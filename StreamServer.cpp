/*++

Program name:

  Apostol Web Service

Module Name:

  StreamServer.cpp

Notices:

  Proccess: Stream Server

Author:

  Copyright (c) Prepodobny Alen

  mailto: alienufo@inbox.ru
  mailto: ufocomp@gmail.com

--*/

#include "Core.hpp"
#include "StreamServer.hpp"
//----------------------------------------------------------------------------------------------------------------------

#define PROVIDER_APPLICATION_NAME "service"
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

            m_Agent = "Streaming Server";
            m_Host = CApostolModule::GetIPByHostName(CApostolModule::GetHostName());

            const auto now = Now();

            m_AuthDate = now;

            m_HeartbeatInterval = 5000;
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::InitializeStreamServer(const CString &Title) {
            
            m_Server.ServerName() = Title;
            m_Server.PollStack(PQServer().PollStack());

            m_Server.DefaultIP() = Config()->Listen();
            m_Server.DefaultPort(Config()->IniFile().ReadInteger(CONFIG_SECTION_NAME, "port", Config()->Port()));

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

            Log()->Debug(0, MSG_PROCESS_START, GetProcessName(), Application()->Header().c_str());

            InitSignals();

            Reload();

            SetUser(Config()->User(), Config()->Group());

            InitializeStreamServer(Application()->Title());
            InitializePQServer(Application()->Title());

            PQServerStart("helper");

            SigProcMask(SIG_UNBLOCK, SigAddSet(&set));

            SetTimerInterval(1000);
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::AfterRun() {
            CApplicationProcess::AfterRun();
            PQServerStop();
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::Run() {

            try {
                m_Server.ActiveLevel(alActive);

                while (!sig_exiting) {

                    log_debug0(APP_LOG_DEBUG_EVENT, Log(), 0, "stream server process cycle");

                    try {
                        m_Server.Wait();
                    } catch (std::exception &e) {
                        Log()->Error(APP_LOG_ERR, 0, "%s", e.what());
                    }

                    if (sig_terminate || sig_quit) {
                        if (sig_quit) {
                            sig_quit = 0;
                            Log()->Error(APP_LOG_NOTICE, 0, "gracefully shutting down");
                            Application()->Header("stream server process is shutting down");
                        }

                        if (!sig_exiting) {
                            sig_exiting = 1;
                            Log()->Error(APP_LOG_NOTICE, 0, "exiting stream server process");
                        }
                    }

                    if (sig_reopen) {
                        sig_reopen = 0;

                        Log()->Error(APP_LOG_NOTICE, 0, "stream server reconnect");

                        m_Server.ActiveLevel(alBinding);
                        m_Server.ActiveLevel(alActive);
                    }
                }
            } catch (std::exception &e) {
                Log()->Error(APP_LOG_ERR, 0, "%s", e.what());
                ExitSigAlarm(5 * 1000);
            }

            Log()->Error(APP_LOG_NOTICE, 0, "stop stream server process");
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

                CPQueryResults Result;
                CStringList SQL;

                try {
                    CApostolModule::QueryToResults(APollQuery, Result);
                    const auto &login = Result[0][0];

                    m_Session = login["session"];
                    m_Secret = login["secret"];

                    m_AuthDate = Now() + (CDateTime) 24 / HoursPerDay;
                } catch (Delphi::Exception::Exception &E) {
                    DoError(E);
                }
            };

            auto OnException = [this](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                DoError(E);
            };

            const CString Application(PROVIDER_APPLICATION_NAME);

            const auto &Providers = Server().Providers();
            const auto &Provider = Providers.DefaultValue();

            CStringList SQL;

            try {
                m_ClientId = Provider.ClientId(Application);
                m_ClientSecret = Provider.Secret(Application);

                SQL.Add(CString().Format("SELECT * FROM api.login(%s, %s, %s, %s);",
                                         PQQuoteLiteral(m_ClientId).c_str(),
                                         PQQuoteLiteral(m_ClientSecret).c_str(),
                                         PQQuoteLiteral(m_Agent).c_str(),
                                         PQQuoteLiteral(m_Host).c_str()
                ));

                ExecSQL(SQL, nullptr, OnExecuted, OnException);
            } catch (Delphi::Exception::Exception &E) {
                DoError(E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::Authorize(CStringList &SQL, const CString &Username) {
            SQL.Add(CString().Format("SELECT * FROM api.authorize(%s);",
                                     PQQuoteLiteral(m_Session).c_str()
            ));

            SQL.Add(CString().Format("SELECT * FROM api.su(%s, %s);",
                                     PQQuoteLiteral(Username).c_str(),
                                     PQQuoteLiteral(m_ClientSecret).c_str()
            ));
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::SetArea(CStringList &SQL, const CString &Area) {
            SQL.Add(CString().Format("SELECT * FROM api.set_session_area(%s);",
                                     PQQuoteLiteral(Area).c_str()
            ));
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::Parse(CUDPAsyncServer *Server, CSocketHandle *Socket, const CString &Protocol, const CString &Data) {

            auto OnExecuted = [this, Server, Socket](CPQPollQuery *APollQuery) {

                CPQResult *pResult;
                CString LResult;

                try {
                    for (int I = 0; I < APollQuery->Count(); I++) {
                        pResult = APollQuery->Results(I);

                        if (pResult->ExecStatus() != PGRES_TUPLES_OK)
                            throw Delphi::Exception::EDBError(pResult->GetErrorMessage());

                        if (I < 3)
                            continue;

                        if (!pResult->GetIsNull(0, 0)) {
                            LResult = base64_decode(pResult->GetValue(0, 0));
                            Server->OutputBuffer().Write(LResult.Data(), LResult.Size());
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

            CStringList SQL;

            Authorize(SQL, API_BOT_USERNAME);
            SetArea(SQL, "default");

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
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoTimer(CPollEventHandler *AHandler) {
            uint64_t exp;

            auto LTimer = dynamic_cast<CEPollTimer *> (AHandler->Binding());
            LTimer->Read(&exp, sizeof(uint64_t));

            try {
                DoHeartbeat();
            } catch (Delphi::Exception::Exception &E) {
                DoServerEventHandlerException(AHandler, E);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoError(const Delphi::Exception::Exception &E) {
            const auto now = Now();

            m_Token.Clear();
            m_Session.Clear();
            m_Secret.Clear();

            m_AuthDate = now + (CDateTime) m_HeartbeatInterval / MSecsPerDay;

            Log()->Error(APP_LOG_ERR, 0, "%s", E.what());
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoHeartbeat() {
            const auto now = Now();

            if ((now >= m_AuthDate)) {
                Authentication();
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoRead(CUDPAsyncServer *Sender, CSocketHandle *Socket, CManagedBuffer &Buffer) {
            BYTE byte;
            size_t size, pos;

            ushort length;
            ushort crc;

            CString Stream;
            CString Data;

            Stream.SetLength(Buffer.Size());
            Buffer.Extract(Stream.Data(), Stream.Size());

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

                if ((length == 0) || (length > Stream.Size() - Stream.Position()))
                    break;

                Data.SetLength(length + size);
                Data.WriteBuffer(&length, size);
                Stream.ReadBuffer(Data.Data() + size, length);

                Debug(Socket, Data);

                crc = (Data[length] << 8) | Data[length - 1];

                if (crc != GetCRC16(Data.Data(), Data.Size() - 2))
                    break;

                Parse(Sender, Socket, PROTOCOL_NAME, Data);
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoWrite(CUDPAsyncServer *Server, CSocketHandle *Socket, CSimpleBuffer &Buffer) {
            //DebugMessage("[%s] BIN: %s\n", NowStr, Bin.c_str());
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoException(CTCPConnection *AConnection, const Delphi::Exception::Exception &E) {
            Log()->Error(APP_LOG_ERR, 0, "%s", E.what());
            sig_reopen = 1;
        }
        //--------------------------------------------------------------------------------------------------------------

        CPQPollQuery *CStreamServer::GetQuery(CPollConnection *AConnection) {
            auto LQuery = CServerProcess::GetQuery(AConnection);

            if (Assigned(LQuery)) {
#if defined(_GLIBCXX_RELEASE) && (_GLIBCXX_RELEASE >= 9)
                LQuery->OnPollExecuted([this](auto && APollQuery) { DoPostgresQueryExecuted(APollQuery); });
                LQuery->OnException([this](auto && APollQuery, auto && AException) { DoPostgresQueryException(APollQuery, AException); });
#else
                LQuery->OnPollExecuted(std::bind(&CStreamServer::DoPostgresQueryExecuted, this, _1));
                LQuery->OnException(std::bind(&CStreamServer::DoPostgresQueryException, this, _1, _2));
#endif
            }

            return LQuery;
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
#ifdef _DEBUG
            BYTE ch;

            TCHAR szString[MAX_BUFFER_SIZE / 4 + 1] = {0};
            CString Bin;

            for (int i = 0; i < Stream.Size(); i++) {
                ch = Stream[i];
                if (IsCtl(ch) || (ch >= 128)) {
                    Bin.Append('.');
                } else {
                    Bin.Append(ch);
                }
            }

            CString Hex;
            Hex.SetLength(Stream.Size() * 3);
            ByteToHexStr((LPSTR) Hex.Data(), Hex.Size(), (LPCBYTE) Stream.Data(), Stream.Size(), ' ');

            const auto NowStr = DateTimeToStr(Now(), szString, MAX_BUFFER_SIZE / 4);

            DebugMessage("[%s] [%s:%d] BIN: %s\n", NowStr, Socket->PeerIP(), Socket->PeerPort(), Bin.c_str());
            DebugMessage("[%s] [%s:%d] HEX: %s\n\n", NowStr, Socket->PeerIP(), Socket->PeerPort(), Hex.c_str());
#endif
        }
    }
}

}
