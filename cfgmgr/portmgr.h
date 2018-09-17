#pragma once

#include "dbconnector.h"
#include "netmsg.h"
#include "orch.h"
#include "producerstatetable.h"

#include <map>
#include <set>
#include <string>

namespace swss {

class PortMgr : public Orch, public NetMsg
{
public:
    PortMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames);

    virtual void onMsg(int nlmsg_tryp, struct nl_object *obj);
    using Orch::doTask;
private:
    set<string> m_portList;

    Table m_cfgPortTable;
    Table m_cfgLagTable;
    Table m_statePortTable;
    Table m_stateLagTable;
    ProducerStateTable m_appPortTable;
    ProducerStateTable m_appLagTable;

    void doTask(Consumer &consumer);
    bool setPortMtu(const string &table, const string &alias, const string &mtu);
    bool setPortAdminStatus(const string &alias, const bool up);
    bool isPortStateOk(const string &table, const string &alias);
};

}
