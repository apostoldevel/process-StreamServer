/*++

Program name:

  Apostol Web Service

Module Name:

  StreamServer.hpp

Notices:

  Proccess: Stream Server

Author:

  Copyright (c) Prepodobny Alen

  mailto: alienufo@inbox.ru
  mailto: ufocomp@gmail.com

--*/

#ifndef APOSTOL_STREAMSEREVER_HPP
#define APOSTOL_STREAMSEREVER_HPP
//----------------------------------------------------------------------------------------------------------------------

extern "C++" {

namespace Apostol {

    namespace Processes {

        //--------------------------------------------------------------------------------------------------------------

        //-- CStreamServer ---------------------------------------------------------------------------------------------

        //--------------------------------------------------------------------------------------------------------------

        class CStreamServer: public CProcessCustom {
            typedef CProcessCustom inherited;

        private:

            CUDPAsyncServer m_Server;

            void BeforeRun() override;
            void AfterRun() override;

            void InitializeStreamServer(const CString &Title);

            void Parse(CUDPAsyncServer *Server, CSocketHandle *Socket, const CString &Protocol, const CString &Data);

        protected:

            void DoTimer(CPollEventHandler *AHandler) override;

            void DoHeartbeat();

            void DoRead(CUDPAsyncServer *Server, CSocketHandle *Socket, CManagedBuffer &Buffer);
            void DoWrite(CUDPAsyncServer *Server, CSocketHandle *Socket, CSimpleBuffer &Buffer);

            void DoException(CTCPConnection *AConnection, const Delphi::Exception::Exception &E);
            bool DoExecute(CTCPConnection *AConnection) override;

            void DoPostgresQueryExecuted(CPQPollQuery *APollQuery);
            void DoPostgresQueryException(CPQPollQuery *APollQuery, const Delphi::Exception::Exception &E);

        public:

            explicit CStreamServer(CCustomProcess* AParent, CApplication *AApplication);

            ~CStreamServer() override = default;

            static class CStreamServer *CreateProcess(CCustomProcess *AParent, CApplication *AApplication) {
                return new CStreamServer(AParent, AApplication);
            }

            void Run() override;

            void LoadConfig();

            CPQPollQuery *GetQuery(CPollConnection *AConnection) override;

        };
        //--------------------------------------------------------------------------------------------------------------

    }
}

using namespace Apostol::Processes;
}
#endif //APOSTOL_STREAMSEREVER_HPP
