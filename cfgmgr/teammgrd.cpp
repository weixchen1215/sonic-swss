#include <fstream>

#include "teammgr.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "select.h"

using namespace std;
using namespace swss;

#define SELECT_TIMEOUT 1000

int gBatchSize = 0;
bool gSwssRecord = false;
bool gLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("lagmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting lagmrgd ---");

    try
    {
        vector<string> cfg_port_tables = {
            CFG_LAG_TABLE_NAME,
            CFG_LAG_MEMBER_TABLE_NAME,
        };

        DBConnector cfgDb(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector stateDb(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

        LagMgr lagmgr(&cfgDb, &stateDb, cfg_port_tables);

        vector<Orch *> cfgOrchList = {&lagmgr};

        Select s;
        for (Orch *o: cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        NetLink netlink;
        netlink.registerGroup(RTNLGRP_LINK);
        
        NetDispatcher::getInstance().registerMessageHandler(RTM_NEWLINK, &lagmgr);
        NetDispatcher::getInstance().registerMessageHandler(RTM_DELLINK, &lagmgr);

        s.addSelectable(&netlink);

        while (true)
        {
            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                lagmgr.doTask();
                continue;
            }

            if (sel != (NetLink *)&netlink)
            {
                auto *c = (Executor *)sel;
                c->execute();
            }
        }
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }

    return -1;
}
