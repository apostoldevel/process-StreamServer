/*++

Program name:

  Apostol CRM

Module Name:

  StreamServer.cpp

Notices:

  Process: Stream Server

Author:

  Copyright (c) Prepodobny Alen

  mailto: alienufo@inbox.ru
  mailto: ufocomp@gmail.com

--*/

#include "Core.hpp"
#include "StreamServer.hpp"
//----------------------------------------------------------------------------------------------------------------------

#define SERVICE_APPLICATION_NAME "service"
#define CONFIG_SECTION_NAME "process/StreamServer"
#define PROTOCOL_NAME "LPWAN"

#define API_BOT_USERNAME "apibot"

extern "C++" {

namespace Apostol {

    namespace Processes {

        //--------------------------------------------------------------------------------------------------------------

        //-- CStreamServer ---------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        CStreamServer::CStreamServer(CCustomProcess *AParent, CApplication *AApplication):
                inherited(AParent, AApplication, "stream process") {

            m_Agent = CString().Format("Streaming Server (%s)", Application()->Title().c_str());
            m_Host = CApostolModule::GetIPByHostName(CApostolModule::GetHostName());

            m_AuthDate = 0;

            m_HeartbeatInterval = 5000;
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::InitializeStreamServer(const CString &Title) {
            m_Server.ServerName() = Title;
            m_Server.AllocateEventHandlers(GetPQClient());

            m_Server.DefaultIP() = Config()->Listen();
            m_Server.DefaultPort(Config()->IniFile().ReadInteger(CONFIG_SECTION_NAME, "port", (ushort) Config()->Port()));

#if defined(_GLIBCXX_RELEASE) && (_GLIBCXX_RELEASE >= 9)
            m_Server.OnVerbose([this](auto && Sender, auto && AConnection, auto && AFormat, auto && args) { DoVerbose(Sender, AConnection, AFormat, args); });
            m_Server.OnAccessLog([this](auto && AConnection) { DoAccessLog(AConnection); });
            m_Server.OnException([this](auto && AConnection, auto && AException) { DoException(AConnection, AException); });
            m_Server.OnEventHandlerException([this](auto && AHandler, auto && AException) { DoServerEventHandlerException(AHandler, AException); });
            m_Server.OnNoCommandHandler([this](auto && Sender, auto && AData, auto && AConnection) { DoNoCommandHandler(Sender, AData, AConnection); });

            m_Server.OnRead([this](auto && Server, auto && Socket, auto && Buffer) { DoRead(Server, Socket, Buffer); });
            m_Server.OnWrite([this](auto && Server, auto && Socket, auto && Buffer) { DoWrite(Server, Socket, Buffer); });
#else
            m_Server.OnVerbose(std::bind(&CStreamServer::DoVerbose, this, _1, _2, _3, _4));
            m_Server.OnAccessLog(std::bind(&CStreamServer::DoAccessLog, this, _1));
            m_Server.OnException(std::bind(&CStreamServer::DoException, this, _1, _2));
            m_Server.OnEventHandlerException(std::bind(&CStreamServer::DoServerEventHandlerException, this, _1, _2));
            m_Server.OnNoCommandHandler(std::bind(&CStreamServer::DoNoCommandHandler, this, _1, _2, _3));

            m_Server.OnRead(std::bind(&CStreamServer::DoRead, this, _1, _2, _3));
            m_Server.OnWrite(std::bind(&CStreamServer::DoWrite, this, _1, _2, _3));
#endif
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::BeforeRun() {
            sigset_t set;

            Application()->Header(Application()->Name() + ": stream process");

            Log()->Debug(APP_LOG_DEBUG_CORE, MSG_PROCESS_START, GetProcessName(), Application()->Header().c_str());

            InitSignals();

            Reload();

            SetUser(Config()->User(), Config()->Group());

            InitializePQClients(Application()->Title(), 1, Config()->PostgresPollMin());

            PQClientStart("helper");

            InitializeStreamServer(Application()->Title());

            SigProcMask(SIG_UNBLOCK, SigAddSet(&set));

            SetTimerInterval(1000);
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::AfterRun() {
            CApplicationProcess::AfterRun();
            PQClientsStop();
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::Run() {
            try {
                m_Server.ActiveLevel(alActive);

                while (!sig_exiting) {

                    Log()->Debug(APP_LOG_DEBUG_EVENT, _T("stream server process cycle"));

                    try {
                        m_Server.Wait();
                    } catch (std::exception &e) {
                        Log()->Error(APP_LOG_ERR, 0, _T("%s"), e.what());
                    }

                    if (sig_terminate || sig_quit) {
                        if (sig_quit) {
                            sig_quit = 0;
                            Log()->Debug(APP_LOG_DEBUG_EVENT, _T("gracefully shutting down"));
                            Application()->Header(_T("stream server process is shutting down"));
                        }

                        if (!sig_exiting) {
                            sig_exiting = 1;
                            Log()->Debug(APP_LOG_DEBUG_EVENT, _T("exiting stream server process"));
                        }
                    }

                    if (sig_reopen) {
                        sig_reopen = 0;

                        Log()->Debug(APP_LOG_DEBUG_EVENT, _T("stream server reconnect"));

                        m_Server.ActiveLevel(alBinding);
                        m_Server.ActiveLevel(alActive);
                    }
                }
            } catch (std::exception &e) {
                Log()->Error(APP_LOG_ERR, 0, _T("%s"), e.what());
                ExitSigAlarm(5 * 1000);
            }

            Log()->Debug(APP_LOG_DEBUG_EVENT, _T("stop stream server process"));
        }
        //--------------------------------------------------------------------------------------------------------------

        bool CStreamServer::DoExecute(CTCPConnection *AConnection) {
            return true;
        }
        //--------------------------------------------------------------------------------------------------------------

        ushort CStreamServer::GetCRC16(void *buffer, size_t size) {
            int crc = 0xFFFF;

            for (int i = 0; i < size; i++) {
                crc = crc ^ ((BYTE *) buffer)[i];

                for (int j = 0; j < 8; ++j) {
                    if ((crc & 0x01) == 1)
                        crc = (crc >> 1 ^ 0xA001);
                    else
                        crc >>= 1;
                }
            }

            return (ushort) crc;
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::Reload() {
            CServerProcess::Reload();

            m_AuthDate = Now();
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::Authentication() {

            auto OnExecuted = [this](CPQPollQuery *APollQuery) {

                CPQueryResults pqResults;
                CStringList SQL;

                try {
                    CApostolModule::QueryToResults(APollQuery, pqResults);

                    const auto &login = pqResults[0];
                    const auto &sessions = pqResults[1];

                    const auto &session = login.First()["session"];

                    m_Sessions.Clear();
                    for (int i = 0; i < sessions.Count(); ++i) {
                        m_Sessions.Add(sessions[i]["get_sessions"]);
                    }

                    m_AuthDate = Now() + (CDateTime) 24 / HoursPerDay;

                    SignOut(session);
                } catch (Delphi::Exception::Exception &E) {
                    DoError(E);
                }
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                DoError(E);
            };

            const auto &caProviders = Server().Providers();
            const auto &caProvider = caProviders.DefaultValue();

            const auto &clientId = caProvider.ClientId(SERVICE_APPLICATION_NAME);
            const auto &clientSecret = caProvider.Secret(SERVICE_APPLICATION_NAME);

            CStringList SQL;

            api::login(SQL, clientId, clientSecret, m_Agent, m_Host);

            api::get_session(SQL, API_BOT_USERNAME, m_Agent, m_Host);

            try {
                ExecSQL(SQL, nullptr, OnExecuted, OnException);
            } catch (Delphi::Exception::Exception &E) {
                DoError(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::SignOut(const CString &Session) {
            CStringList SQL;

            api::signout(SQL, Session);

            try {
                ExecSQL(SQL);
            } catch (Delphi::Exception::Exception &E) {
                DoError(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::Heartbeat(CDateTime Now) {
            if ((Now >= m_AuthDate)) {
                Authentication();
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::Parse(CUDPAsyncServer *Server, CSocketHandle *Socket, const CString &Protocol, const CString &Data) {

            auto OnExecuted = [this, Server, Socket](CPQPollQuery *APollQuery) {

                CPQResult *pResult;
                CString Result;

                try {
                    for (int I = 0; I < APollQuery->Count(); I++) {
                        pResult = APollQuery->Results(I);

                        if (pResult->ExecStatus() != PGRES_TUPLES_OK)
                            throw Delphi::Exception::EDBError(pResult->GetErrorMessage());

                        if (I < 3)
                            continue;

                        if (!pResult->GetIsNull(0, 0)) {
                            Result = base64_decode(pResult->GetValue(0, 0));
                            Server->OutputBuffer().Write(Result.Data(), Result.Size());
                            Server->Send(Socket);
                        }
                    }
                } catch (Delphi::Exception::Exception &E) {
                    DoError(E);
                }
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                DoError(E);
            };

            const auto &Base64 = base64_encode(Data);

            for (int i = 0; i < m_Sessions.Count(); ++i) {
                const auto &session = m_Sessions[i];

                CStringList SQL;

                api::authorize(SQL, session);
                api::set_area(SQL);

                SQL.Add(CString().MaxFormatSize(256 + Protocol.Size() + Base64.Size()).
                        Format("SELECT * FROM stream.parse('%s', '%s:%d', '%s');",
                               Protocol.c_str(),
                               Socket->PeerIP(), Socket->PeerPort(),
                               Base64.c_str()
                ));

                try {
                    ExecSQL(SQL, nullptr, OnExecuted, OnException);
                } catch (Delphi::Exception::Exception &E) {
                    DoError(E);
                }
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoTimer(CPollEventHandler *AHandler) {
            uint64_t exp;

            auto pTimer = dynamic_cast<CEPollTimer *> (AHandler->Binding());
            pTimer->Read(&exp, sizeof(uint64_t));

            try {
                Heartbeat(AHandler->TimeStamp());
            } catch (Delphi::Exception::Exception &E) {
                DoServerEventHandlerException(AHandler, E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoError(const Delphi::Exception::Exception &E) {
            m_AuthDate = Now() + (CDateTime) m_HeartbeatInterval / MSecsPerDay;
            Log()->Error(APP_LOG_ERR, 0, "%s", E.what());
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoRead(CUDPAsyncServer *Sender, CSocketHandle *Socket, CManagedBuffer &Buffer) {

            BYTE byte;
            size_t size, streamSize;

            ushort length;
            ushort crc, crcData;

            CString Stream;
            CString Data;

            Stream.SetLength(Buffer.Size());
            Buffer.Extract(Stream.Data(), Stream.Size());

            Log()->Stream("[%s:%d] DoRead:", Socket->PeerIP(), Socket->PeerPort());
            Debug(Socket, Stream);

            while (Stream.Position() < Stream.Size()) {
                Data.Clear();
                size = 0;

                Stream.ReadBuffer(&byte, sizeof(byte));
                size++;

                // length – длина данных (1 или 2 байта).
                // 1 байт: 0-6 бит – младшие биты длины, 7 бит – длина данных 2 байта).
                // 2 байт: присутствует если установлен 7 бит первого байта, 0-7 бит – старшие биты длины.
                length = byte;
                if ((length & 0x80) == 0x80) {
                    Stream.ReadBuffer(&byte, sizeof(byte));
                    size++;

                    length &= ~(1 << 7);
                    length = (length << 8) | byte;
                }

                streamSize = Stream.Size() - Stream.Position();

                if ((length == 0) || (length > streamSize)) {
                    Data.SetLength(size + streamSize);
                    Data.WriteBuffer(&length, size);

                    if (streamSize > 0) {
                        Data.WriteBuffer(&length, size);
                        Stream.ReadBuffer(Data.Data() + size, streamSize);
                    }

                    Log()->Stream("[%s:%d] Incorrect:", Socket->PeerIP(), Socket->PeerPort());
                    Debug(Socket, Data);

                    Log()->Stream("[%s:%d] [%d:%d:%d] [%d] Incorrect length.", Socket->PeerIP(), Socket->PeerPort(), Stream.Size(), Stream.Position(), streamSize, length);
                    break;
                }

                Data.SetLength(size + length);
                Data.WriteBuffer(&length, size);
                Stream.ReadBuffer(Data.Data() + size, length);

                Log()->Stream("[%s:%d] Data:", Socket->PeerIP(), Socket->PeerPort());
                Debug(Socket, Data);

                crc = ((BYTE) Data[length] << 8) | (BYTE) Data[length - 1];
                crcData = GetCRC16(Data.Data(), Data.Size() - 2);

                if (crc != crcData) {
                    Log()->Stream("[%s:%d] [%d] [%d] [%d] Invalid CRC.", Socket->PeerIP(), Socket->PeerPort(), crc, crcData);
                    break;
                }

                Parse(Sender, Socket, PROTOCOL_NAME, Data);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoWrite(CUDPAsyncServer *Server, CSocketHandle *Socket, CSimpleBuffer &Buffer) {
            if (Buffer.Size() > 0) {
                CString Stream;

                Stream.SetLength(Buffer.Size());
                Buffer.Extract(Stream.Data(), Stream.Size());

                Log()->Stream("[%s:%d] DoWrite:", Socket->PeerIP(), Socket->PeerPort());
                Debug(Socket, Stream);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoException(CTCPConnection *AConnection, const Delphi::Exception::Exception &E) {
            Log()->Error(APP_LOG_ERR, 0, "%s", E.what());
            sig_reopen = 1;
        }
        //--------------------------------------------------------------------------------------------------------------

        CPQPollQuery *CStreamServer::GetQuery(CPollConnection *AConnection) {
            auto pQuery = CServerProcess::GetQuery(AConnection);

            if (Assigned(pQuery)) {
#if defined(_GLIBCXX_RELEASE) && (_GLIBCXX_RELEASE >= 9)
                pQuery->OnPollExecuted([this](auto && APollQuery) { DoPostgresQueryExecuted(APollQuery); });
                pQuery->OnException([this](auto && APollQuery, auto && AException) { DoPostgresQueryException(APollQuery, AException); });
#else
                pQuery->OnPollExecuted(std::bind(&CStreamServer::DoPostgresQueryExecuted, this, _1));
                pQuery->OnException(std::bind(&CStreamServer::DoPostgresQueryException, this, _1, _2));
#endif
            }

            return pQuery;
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoPostgresQueryExecuted(CPQPollQuery *APollQuery) {
            CPQResult *pResult;

            try {
                for (int I = 0; I < APollQuery->Count(); I++) {
                    pResult = APollQuery->Results(I);

                    if (pResult->ExecStatus() != PGRES_TUPLES_OK)
                        throw Delphi::Exception::EDBError(pResult->GetErrorMessage());

                    if (!pResult->GetIsNull(0, 1) && SameText(pResult->GetValue(0, 1), "f"))
                        throw Delphi::Exception::EDBError(pResult->GetValue(0, 2));
                }
            } catch (std::exception &e) {
                Log()->Error(APP_LOG_ERR, 0, "%s", e.what());
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoPostgresQueryException(CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
            Log()->Error(APP_LOG_ERR, 0, "%s", E.what());
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::Debug(CSocketHandle *Socket, const CString &Stream) {
            BYTE ch;

            CString Bin;

            for (int i = 0; i < Stream.Size(); i++) {
                ch = Stream[i];
                if (IsCtl(ch) || (ch >= 128)) {
                    Bin.Append('.');
                } else {
                    Bin.Append((TCHAR) ch);
                }
            }

            CString Hex;
            Hex.SetLength(Stream.Size() * 3);
            ByteToHexStr((LPSTR) Hex.Data(), Hex.Size(), (LPCBYTE) Stream.Data(), Stream.Size(), ' ');

            Log()->Stream("[%s:%d] BIN: %d: %s", Socket->PeerIP(), Socket->PeerPort(), Stream.Size(), Bin.c_str());
            Log()->Stream("[%s:%d] HEX: %s", Socket->PeerIP(), Socket->PeerPort(), Hex.c_str());
        }
    }
}

}
