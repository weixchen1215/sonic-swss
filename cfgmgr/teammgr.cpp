#include <netlink/route/link.h>

#include "exec.h"
#include "teammgr.h"
#include "logger.h"
#include "shellcmd.h"
#include "tokenize.h"

#include <sstream>

using namespace std;
using namespace swss;

LagMgr::LagMgr(DBConnector *cfgDb, DBConnector *stateDb, const vector<TableConnector> &tables) :
    Orch(tables),
    m_cfgLagTable(cfgDb, CFG_LAG_TABLE_NAME),
    m_cfgLagMemberTable(cfgDb, CFG_LAG_MEMBER_TABLE_NAME),
    m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
    m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME)
{
    // Remove all state database LAG entries
    vector<string> keys;
    m_stateLagTable.getKeys(keys);

    for (string alias : keys)
    {
        m_stateLagTable.del(alias);
    }
}

void LagMgr::onMsg(int nlmsg_type, struct nl_object *obj)
{

     struct rtnl_link *link = (struct rtnl_link *)obj;
    string alias = rtnl_link_get_name(link);
    if (nlmsg_type == RTM_NEWLINK)
    {
        if (m_portList.find(alias) != m_portList.end())
        {
            return;
        }

        if (alias.find("Ethernet") == 0)
        {
            SWSS_LOG_NOTICE("get prot %s", alias.c_str());

            vector<string> keys;
            m_cfgLagMemberTable.getKeys(keys);

            for (auto key : keys)
            {
                auto tokens = tokenize(key, '|');

                auto lag = tokens[0];
                auto member = tokens[1];

                if (alias == member)
                {
                    // port must already be state ok
                    // lag must already be state ok
                    addLagMember(lag, alias);
                    m_portList.insert(alias);
                    return;
                }
            }
        }
    }
    else
    {
        if (m_portList.find(alias) != m_portList.end())
        {
            SWSS_LOG_NOTICE("Remove %s from post list", alias.c_str());
            m_portList.erase(alias);
        }
    }
}

bool LagMgr::isPortStateOk(const string &alias)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> temp;

    if (!m_statePortTable.get(alias, temp))
    {
        SWSS_LOG_NOTICE("Lag %s is not ready", alias.c_str());
        return false;
    }

    return true;
}

bool LagMgr::isLagStateOk(const string &alias)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> temp;

    if (!m_stateLagTable.get(alias, temp))
    {
        SWSS_LOG_NOTICE("Lag %s is not ready", alias.c_str());
        return false;
    }

    return true;
}

void LagMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);

    SWSS_LOG_NOTICE("here do task! %s", alias.c_str());

        if (op == SET_COMMAND)
        {
            if (table == CFG_LAG_TABLE_NAME)
            {
                int min_links = 0;
                bool fall_back = false;

                for (auto i : kfvFieldsValues(t))
                {
                    // min_links and fall_back attributes cannot be changed
                    // after the LAG is created.
                    if (fvField(i) == "min_links")
                    {
                        min_links = stoi(fvValue(i));
                    }
                    else if (fvField(i) == "fall_back")
                    {
                        fall_back = fvValue(i) == "true";
                    }
                }

                addLag(alias, min_links, fall_back);
            }
            else if (table == CFG_LAG_MEMBER_TABLE_NAME)
            {
                SWSS_LOG_NOTICE("key: %s", alias.c_str());
                auto tokens = tokenize(alias, '|');

                auto lag = tokens[0];
                auto member = tokens[1];

                if (!isLagStateOk(lag))
                {
                    it++;
                    continue;
                }

                addLagMember(lag, member);
                m_portList.insert(member);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (table == CFG_LAG_TABLE_NAME)
            {
                removeLag(alias);
            }
            else if (table == CFG_LAG_MEMBER_TABLE_NAME)
            {
                SWSS_LOG_NOTICE("key: %s", alias.c_str());
                auto tokens = tokenize(alias, '|');

                auto lag = tokens[0];
                auto member = tokens[1];

                removeLagMember(lag, member);
                m_portList.erase(member);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool LagMgr::addLag(const string &alias, int min_links, bool fall_back)
{
    stringstream cmd;
    string res;

    string mac = "d8:9e:f3:d6:90:e0";
    
    string conf = "'{\"device\":\"" + alias + "\","
        "\"hwaddr\":\"" + mac + "\","
        "\"runner\":{"
            "\"active\":\"true\","
            "\"name\":\"lacp\""
        "}"
    "}'";

    cmd << TEAMD_CMD << " -r -t " << alias << " -c " << conf << " -d";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    return true;
}

bool LagMgr::removeLag(const string &alias)
{
    stringstream cmd;
    string res;

    cmd << TEAMD_CMD << " -k -t " << alias;
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    m_stateLagTable.del(alias);

    return true;
}

bool LagMgr::addLagMember(const string &lag, const string &member)
{
    stringstream cmd;
    string res;

    // TODO admin down lag member first

    cmd << TEAMDCTL_CMD << " " << lag << " port add " << member;
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    SWSS_LOG_NOTICE("add lag member");
    // TODO apply MTU configurations from master etc

    return true;
}

bool LagMgr::removeLagMember(const string &lag, const string &member)
{
    stringstream cmd;
    string res;

    cmd << TEAMDCTL_CMD << " " << lag << " port remove " << member;
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    // TODO apply port original configurations

    SWSS_LOG_NOTICE("remove lag member");

    return true;
}
