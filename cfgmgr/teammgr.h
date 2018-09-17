#pragma once

#include <set>
#include <string>

#include "dbconnector.h"
#include "netmsg.h"
#include "orch.h"
#include "producerstatetable.h"

// TODO: change name to teammgrd

namespace swss {

class LagMgr : public Orch, public NetMsg
{
public:
    LagMgr(DBConnector *cfgDb, DBConnector *stateDb, const vector<TableConnector> &tables);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);
    using Orch::doTask;
private:
    Table m_cfgLagTable;
    Table m_cfgLagMemberTable;
    Table m_statePortTable;
    Table m_stateLagTable;

    set<string> m_portList;

    void doTask(Consumer &consumer);

    bool addLag(const string &alias, int min_links, bool fall_back);
    bool removeLag(const string &alias);
    bool addLagMember(const string &lag, const string &member);
    bool removeLagMember(const string &lag, const string &member);

    void recover();
    bool isPortStateOk(const string&);
    bool isLagStateOk(const string&);
};

}
