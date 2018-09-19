#include "exec.h"
#include "teammgr.h"
#include "logger.h"
#include "shellcmd.h"
#include "tokenize.h"

#include <sstream>
#include <thread>

using namespace std;
using namespace swss;

LagMgr::LagMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *staDb, const vector<TableConnector> &tables) :
    Orch(tables),
    m_cfgPortTable(cfgDb, CFG_PORT_TABLE_NAME),
    m_cfgLagTable(cfgDb, CFG_LAG_TABLE_NAME),
    m_cfgLagMemberTable(cfgDb, CFG_LAG_MEMBER_TABLE_NAME),
    m_appPortTable(appDb, APP_PORT_TABLE_NAME),
    m_appLagTable(appDb, APP_LAG_TABLE_NAME),
    m_statePortTable(staDb, STATE_PORT_TABLE_NAME),
    m_stateLagTable(staDb, STATE_LAG_TABLE_NAME)
{
    // Remove all state database LAG entries
    vector<string> keys;
    m_stateLagTable.getKeys(keys);

    for (auto alias : keys)
    {
        m_stateLagTable.del(alias);
    }
}

bool LagMgr::isPortStateOk(const string &alias)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> temp;

    if (!m_statePortTable.get(alias, temp))
    {
        SWSS_LOG_INFO("Port %s is not ready", alias.c_str());
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
        SWSS_LOG_INFO("Lag %s is not ready", alias.c_str());
        return false;
    }

    return true;
}

void LagMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    if (table == CFG_LAG_TABLE_NAME)
    {
        doLagTask(consumer);
    }
    else if (table == CFG_LAG_MEMBER_TABLE_NAME)
    {
        doLagMemberTask(consumer);
    }
    else if (table == STATE_PORT_TABLE_NAME)
    {
        doPortUpdateTask(consumer);
    }
}

void LagMgr::doLagTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);

    SWSS_LOG_NOTICE("here do task! %s", alias.c_str());

        if (op == SET_COMMAND)
        {
                int min_links = 0;
                bool fallback = false;
                bool admin_status = true;
                string mtu;

                for (auto i : kfvFieldsValues(t))
                {
                    // min_links and fallback attributes cannot be changed
                    // after the LAG is created.
                    if (fvField(i) == "min_links")
                    {
                        min_links = stoi(fvValue(i));
                        SWSS_LOG_NOTICE("get min_links value is %d", min_links);
                    }
                    else if (fvField(i) == "fallback")
                    {
                        fallback = fvValue(i) == "true";
                    }
                    else if (fvField(i) == "admin_status")
                    {
                        admin_status = fvValue(i) == "up";
                    }
                    else if (fvField(i) == "mtu")
                    {
                        mtu = fvValue(i);
                        SWSS_LOG_NOTICE("get mtu value is %s", mtu.c_str());
                    }
                }

                if (m_lagList.find(alias) == m_lagList.end())
                {
                    addLag(alias, min_links, fallback);
                    m_lagList.insert(alias);
                }

                setLagAdminStatus(alias, admin_status);
                setLagMtu(alias, mtu);
        }
        else if (op == DEL_COMMAND)
        {
            if (m_lagList.find(alias) != m_lagList.end())
            {
                removeLag(alias);
                m_lagList.erase(alias);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

void LagMgr::doLagMemberTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        auto tokens = tokenize(kfvKey(t), '|');
        auto lag = tokens[0];
        auto member = tokens[1];

        auto op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            if (!isPortStateOk(member) || !isLagStateOk(lag))
            {
                it++;
                continue;
            }

            if (m_portList.find(member) == m_portList.end())
            {
                addLagMember(lag, member);
                m_portList.insert(member);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_portList.find(member) != m_portList.end())
            {
                removeLagMember(lag, member);
                m_portList.erase(member);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

void LagMgr::doPortUpdateTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        auto alias = kfvKey(t);
        auto op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            if (m_portList.find(alias) == m_portList.end())
            {
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
                    }
                }
            }

        }
        else if (op == DEL_COMMAND)
        {
            if (m_portList.find(alias) != m_portList.end())
            {
                SWSS_LOG_NOTICE("Remove %s from post list", alias.c_str());
                m_portList.erase(alias);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool LagMgr::setLagAdminStatus(const string &alias, const bool up)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link set dev " << alias << (up ? " up" : " down");
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    return true;
}

bool LagMgr::setLagMtu(const string &alias, const string &mtu)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link set dev " << alias << " mtu " << mtu;
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    vector<FieldValueTuple> fvs;
    FieldValueTuple fv("mtu", mtu);
    fvs.push_back(fv);
    m_appLagTable.set(alias, fvs);

    vector<string> keys;
    m_cfgLagMemberTable.getKeys(keys);

    for (auto key : keys)
    {
        auto tokens = tokenize(key, '|');
        auto lag = tokens[0];
        auto member = tokens[1];

        if (alias == lag)
        {
            m_appPortTable.set(member, fvs);
        }
    }

    return true;
}

bool LagMgr::addLag(const string &alias, int min_links, bool fallback)
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

    // admin down lag member first
    cmd << IP_CMD << " link set dev " << member << " down; ";
    cmd << TEAMDCTL_CMD << " " << lag << " port add " << member << "; ";

    vector<FieldValueTuple> fvs;
    m_cfgPortTable.get(member, fvs);
    
    // set to up by default
    bool up = true;
    for (auto i : fvs)
    {
        if (fvField(i) == "admin_status")
        {
            up = fvValue(i) == "up";
            SWSS_LOG_NOTICE("the status of the admin status is %s", fvValue(i).c_str());
        }
    }

    m_cfgLagTable.get(lag, fvs);

    string mtu = "9100";
    for (auto i : fvs)
    {
        if (fvField(i) == "mtu")
        {
            mtu = fvValue(i);
        }
    }

        cmd << IP_CMD << " link set dev " << member << (up ? " up" : " down");
    EXEC_WITH_ERROR_THROW(cmd.str(), res);
    SWSS_LOG_NOTICE("Add %s to port channel %s", member.c_str(), lag.c_str());

    fvs.clear();
    FieldValueTuple fv("admin_status", (up ? "up" : "down"));
    fvs.push_back(fv);
    fv = FieldValueTuple("mtu", mtu);
    fvs.push_back(fv);
    m_appPortTable.set(member, fvs);

    return true;
}

bool LagMgr::removeLagMember(const string &lag, const string &member)
{
    stringstream cmd;
    string res;

    cmd << TEAMDCTL_CMD << " " << lag << " port remove " << member << "; ";


    vector<FieldValueTuple> fvs;
    m_cfgPortTable.get(member, fvs);

    bool up = true;
    string mtu = "9100";
    for (auto i : fvs)
    {
        if (fvField(i) == "admin_status")
        {
            up = fvValue(i) == "up";
        }
        else if (fvField(i) == "mtu")
        {
            mtu = fvValue(i);
        }
    }

    cmd << IP_CMD << " link set dev " << member << (up ? " up; " : " down; ");
    cmd << IP_CMD << " link set dev " << member << " mtu " << mtu;

    EXEC_WITH_ERROR_THROW(cmd.str(), res);
    fvs.clear();
    FieldValueTuple fv("admin_status", (up ? "up" : "down"));
    fvs.push_back(fv);
    fv = FieldValueTuple("mtu", mtu);
    fvs.push_back(fv);
    m_appPortTable.set(member, fvs);

    SWSS_LOG_NOTICE("Remove %s from port channel %s", member.c_str(), lag.c_str());

    return true;
}
