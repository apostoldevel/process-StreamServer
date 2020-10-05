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

extern "C++" {

namespace Apostol {

    namespace Processes {

        //--------------------------------------------------------------------------------------------------------------

        //-- CStreamServer ---------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        CStreamServer::CStreamServer(CCustomProcess *AParent, CApplication *AApplication):
                inherited(AParent, AApplication, "stream process") {

        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::LoadConfig() {

        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::InitializeStreamServer(const CString &Title) {
            
            m_Server.ServerName() = Title;

            m_Server.DefaultIP() = Config()->Listen();
            m_Server.DefaultPort(Config()->IniFile().ReadInteger("stream", "port", Config()->Port()));

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

            Config()->Reload();

            LoadConfig();

            SetUser(Config()->User(), Config()->Group());

            InitializeStreamServer(Application()->Title());
            InitializePQServer(Application()->Title());

            PQServer().PollStack(m_Server.PollStack());
            PQServerStart("helper");

            SigProcMask(SIG_UNBLOCK, SigAddSet(&set));
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
                        Log()->Error(APP_LOG_EMERG, 0, e.what());
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
                Log()->Error(APP_LOG_EMERG, 0, e.what());
                ExitSigAlarm(5 * 1000);
            }

            Log()->Error(APP_LOG_NOTICE, 0, "stop stream server process");
        }
        //--------------------------------------------------------------------------------------------------------------

        bool CStreamServer::DoExecute(CTCPConnection *AConnection) {
            return true;
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::Parse(CUDPAsyncServer *Server, CSocketHandle *Socket, const CString &Protocol, const CString &Data) {

            auto OnExecuted = [Server, Socket](CPQPollQuery *APollQuery) {

                CPQResult *Result;
                CString LResult;

                try {
                    for (int I = 0; I < APollQuery->Count(); I++) {
                        Result = APollQuery->Results(I);

                        if (Result->ExecStatus() != PGRES_TUPLES_OK)
                            throw Delphi::Exception::EDBError(Result->GetErrorMessage());

                        if (!Result->GetIsNull(0, 0)) {
                            LResult = base64_decode(Result->GetValue(0, 0));
                            Server->OutputBuffer().Write(LResult.Data(), LResult.Size());
                            Server->Send(Socket);
                        }
                    }
                } catch (std::exception &e) {
                    Log()->Error(APP_LOG_EMERG, 0, e.what());
                }
            };

            auto OnException = [](CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
                Log()->Error(APP_LOG_EMERG, 0, E.what());
            };

            const auto &Base64 = base64_encode(Data);

            CStringList SQL;

            SQL.Add(CString().MaxFormatSize(256 + Protocol.Size() + Base64.Size()).
                Format("SELECT * FROM stream.parse('%s', '%s:%d', '%s');",
                     Protocol.c_str(),
                     Socket->PeerIP(), Socket->PeerPort(),
                     Base64.c_str()
            ));

            try {
                ExecSQL(SQL, nullptr, OnExecuted, OnException);
            } catch (std::exception &e) {
                Log()->Error(APP_LOG_EMERG, 0, e.what());
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

        void CStreamServer::DoHeartbeat() {

        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoRead(CUDPAsyncServer *Sender, CSocketHandle *Socket, CManagedBuffer &Buffer) {
            BYTE ch;
            TCHAR szString[MAX_BUFFER_SIZE / 4 + 1] = {0};

            CString Stream;
            Stream.SetLength(Buffer.Size());
            Buffer.Extract(Stream.Data(), Stream.Size());

            Parse(Sender, Socket, "LPWAN", Stream);

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
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoWrite(CUDPAsyncServer *Server, CSocketHandle *Socket, CSimpleBuffer &Buffer) {
            //DebugMessage("[%s] BIN: %s\n", NowStr, Bin.c_str());
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoException(CTCPConnection *AConnection, const Delphi::Exception::Exception &E) {
            Log()->Error(APP_LOG_EMERG, 0, E.what());
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
            CPQResult *Result;

            try {
                for (int I = 0; I < APollQuery->Count(); I++) {
                    Result = APollQuery->Results(I);

                    if (Result->ExecStatus() != PGRES_TUPLES_OK)
                        throw Delphi::Exception::EDBError(Result->GetErrorMessage());

                    if (!Result->GetIsNull(0, 1) && SameText(Result->GetValue(0, 1), "f"))
                        throw Delphi::Exception::EDBError(Result->GetValue(0, 2));
                }
            } catch (std::exception &e) {
                Log()->Error(APP_LOG_EMERG, 0, e.what());
            }
        }
        //--------------------------------------------------------------------------------------------------------------

        void CStreamServer::DoPostgresQueryException(CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E) {
            Log()->Error(APP_LOG_EMERG, 0, E.what());
        }
        //--------------------------------------------------------------------------------------------------------------
    }
}

}
