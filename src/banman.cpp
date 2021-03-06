// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <banman.h>

#include <netaddress.h>
#include <ui_interface.h>
#include <util/system.h>
#include <util/time.h>
#include <util/translation.h>

#include <algorithm>


BanMan::BanMan(fs::path ban_file, CClientUIInterface* client_interface, int64_t default_ban_time)
    : m_client_interface(client_interface), m_ban_db(std::move(ban_file)), m_default_ban_time(default_ban_time)
{
    if (m_client_interface) m_client_interface->InitMessage(_("Loading banlist...").translated);

    int64_t n_start = GetTimeMillis();
    m_is_dirty = false;
    banmap_t banmap;
    if (m_ban_db.Read(banmap)) {
        SetBanned(banmap);        // thread save setter
        SetBannedSetDirty(false); // no need to write down, just read data
        SweepBanned();            // sweep out unused entries

        LogPrint(BCLog::NET, "Loaded %d banned node ips/subnets from banlist.dat  %dms\n",
            banmap.size(), GetTimeMillis() - n_start);
    } else {
        LogPrintf("Invalid or missing banlist.dat; recreating\n");
        SetBannedSetDirty(true); // force write
        DumpBanlist();
    }
}

BanMan::~BanMan()
{
    DumpBanlist();
}

void BanMan::SetMisbehavingLimit(const size_t limit)
{
    LOCK(m_cs_banned);
    // NOTE: For now, this only works before bans are set!
    m_misbehaving_addrs.set_capacity(limit);
}

void BanMan::DumpBanlist()
{
    SweepBanned(); // clean unused entries (if bantime has expired)

    if (!BannedSetIsDirty()) return;

    int64_t n_start = GetTimeMillis();

    banmap_t banmap;
    GetBanned(banmap);
    if (m_ban_db.Write(banmap)) {
        SetBannedSetDirty(false);
    }

    LogPrint(BCLog::NET, "Flushed %d banned node ips/subnets to banlist.dat  %dms\n",
        banmap.size(), GetTimeMillis() - n_start);
}

void BanMan::ClearBanned()
{
    {
        LOCK(m_cs_banned);
        m_banned_addrs.clear();
        m_misbehaving_addrs.clear();
        m_banned_subnets.clear();
        m_is_dirty = true;
    }
    DumpBanlist(); //store banlist to disk
    if (m_client_interface) m_client_interface->BannedListChanged();
}

int BanMan::IsBannedLevel(CNetAddr net_addr)
{
    // Returns the most severe level of banning that applies to this address.
    // 0 - Not banned
    // 1 - Automatic misbehavior ban
    // 2 - Any other ban
    int level = 0;
    auto current_time = GetTime();
    LOCK(m_cs_banned);
    const auto addr_ban_entry = m_banned_addrs.find(net_addr);
    if (addr_ban_entry != m_banned_addrs.end()) {
        const CBanEntry& ban_entry = addr_ban_entry->second;

        if (current_time < ban_entry.nBanUntil) {
            if (ban_entry.banReason != BanReasonNodeMisbehaving) return 2;
            level = 1;
        }
    }
    for (const auto& it : m_banned_subnets) {
        CSubNet sub_net = it.first;
        CBanEntry ban_entry = it.second;

        if (current_time < ban_entry.nBanUntil && sub_net.Match(net_addr)) {
            if (ban_entry.banReason != BanReasonNodeMisbehaving) return 2;
            level = 1;
        }
    }
    return level;
}

bool BanMan::IsBanned(CNetAddr net_addr)
{
    auto current_time = GetTime();
    LOCK(m_cs_banned);
    {
        const auto it = m_banned_addrs.find(net_addr);
        if (it != m_banned_addrs.end()) {
            CBanEntry ban_entry = it->second;
            if (current_time < ban_entry.nBanUntil) {
                return true;
            }
        }
    }
    for (const auto& it : m_banned_subnets) {
        CSubNet sub_net = it.first;
        CBanEntry ban_entry = it.second;

        if (current_time < ban_entry.nBanUntil && sub_net.Match(net_addr)) {
            return true;
        }
    }
    return false;
}

bool BanMan::IsBanned(CSubNet sub_net)
{
    const CNetAddr* addr;
    if (sub_net.IsSingleAddr(&addr)) {
        return IsBanned(*addr);
    }
    auto current_time = GetTime();
    LOCK(m_cs_banned);
    banmap_t::iterator i = m_banned_subnets.find(sub_net);
    if (i != m_banned_subnets.end()) {
        CBanEntry ban_entry = (*i).second;
        if (current_time < ban_entry.nBanUntil) {
            return true;
        }
    }
    return false;
}

void BanMan::Ban(const CNetAddr& net_addr, const BanReason& ban_reason, int64_t ban_time_offset, bool since_unix_epoch)
{
    CSubNet sub_net(net_addr);
    Ban(sub_net, ban_reason, ban_time_offset, since_unix_epoch);
}

void BanMan::Ban(const CSubNet& sub_net, const BanReason& ban_reason, int64_t ban_time_offset, bool since_unix_epoch)
{
    CBanEntry ban_entry(GetTime(), ban_reason);

    int64_t normalized_ban_time_offset = ban_time_offset;
    bool normalized_since_unix_epoch = since_unix_epoch;
    if (ban_time_offset <= 0) {
        normalized_ban_time_offset = m_default_ban_time;
        normalized_since_unix_epoch = false;
    }
    ban_entry.nBanUntil = (normalized_since_unix_epoch ? 0 : GetTime()) + normalized_ban_time_offset;

    {
        LOCK(m_cs_banned);
        const CNetAddr *addr = nullptr;
        const bool is_single_addr = sub_net.IsSingleAddr(&addr);
        auto& old_ban_entry = is_single_addr ? m_banned_addrs[*addr] : m_banned_subnets[sub_net];
        if (old_ban_entry.banReason == BanReasonManuallyAdded && ban_reason != BanReasonManuallyAdded) return;
        const bool ban_reason_upgrade = (old_ban_entry.banReason == BanReasonNodeMisbehaving && ban_reason != BanReasonNodeMisbehaving);
        if (old_ban_entry.nBanUntil < ban_entry.nBanUntil || ban_reason_upgrade) {
            if (m_misbehaving_addrs.capacity()) {
                // we have a limit on misbehaving entries
                if (old_ban_entry.nBanUntil) {
                    // overwriting a prior ban
                    if (ban_reason_upgrade) {
                        // overwriting a misbehaving entry with manually-added
                        // ensure we won't remove a manual ban later
                        assert(is_single_addr);
                        m_misbehaving_addrs.erase(std::find(m_misbehaving_addrs.begin(), m_misbehaving_addrs.end(), *addr));
                    }
                } else if (ban_reason == BanReasonNodeMisbehaving) {
                    // completely new misbehaving entry
                    assert(is_single_addr);
                    if (m_misbehaving_addrs.full()) {
                        auto old_misbehaving = m_misbehaving_addrs.front();
                        CSubNet old_misbehaving_sub_net(old_misbehaving);
                        LogPrint(BCLog::NET, "%s: Removed banned node ip/subnet from banlist.dat: %s\n", __func__, old_misbehaving_sub_net.ToString() + " (misbehaving ban overflow)");
                        m_banned_addrs.erase(old_misbehaving);
                        // push_back will overwrite
                    }
                    m_misbehaving_addrs.push_back(*addr);
                }
            }
            old_ban_entry = ban_entry;
            m_is_dirty = true;
        } else
            return;
    }
    if (m_client_interface) m_client_interface->BannedListChanged();

    //store banlist to disk immediately if user requested ban
    if (ban_reason == BanReasonManuallyAdded) DumpBanlist();
}

bool BanMan::Unban(const CNetAddr& net_addr)
{
    CSubNet sub_net(net_addr);
    return Unban(sub_net);
}

bool BanMan::Unban(const CSubNet& sub_net)
{
    {
        LOCK(m_cs_banned);
        const CNetAddr *addr;
        if (sub_net.IsSingleAddr(&addr)) {
            auto it = m_banned_addrs.find(*addr);
            if (it == m_banned_addrs.end()) return false;
            if (it->second.banReason == BanReasonNodeMisbehaving) {
                m_misbehaving_addrs.erase(std::find(m_misbehaving_addrs.begin(), m_misbehaving_addrs.end(), *addr));
            }
            m_banned_addrs.erase(it);
        } else {
            if (m_banned_subnets.erase(sub_net) == 0) return false;
        }
        m_is_dirty = true;
    }
    if (m_client_interface) m_client_interface->BannedListChanged();
    DumpBanlist(); //store banlist to disk immediately
    return true;
}

void BanMan::GetBanned(banmap_t& banmap)
{
    LOCK(m_cs_banned);
    // Sweep the banlist so expired bans are not returned
    SweepBanned();
    banmap = m_banned_subnets; //create a thread safe copy
    for (const auto& addr_pair : m_banned_addrs) {
        banmap[CSubNet(addr_pair.first)] = addr_pair.second;
    }
}

void BanMan::SetBanned(const banmap_t& banmap)
{
    LOCK(m_cs_banned);
    m_banned_addrs.clear();
    m_banned_subnets.clear();
    const CNetAddr* addr;
    for (const auto& sub_net_pair : banmap) {
        const auto& sub_net = sub_net_pair.first;
        const auto& ban_entry = sub_net_pair.second;
        if (sub_net.IsSingleAddr(&addr)) {
            m_banned_addrs[*addr] = ban_entry;
        } else {
            m_banned_subnets[sub_net] = ban_entry;
        }
    }
    m_is_dirty = true;
}

void BanMan::SweepBanned()
{
    int64_t now = GetTime();
    bool notify_ui = false;
    {
        LOCK(m_cs_banned);
        banmap_t::iterator it = m_banned_subnets.begin();
        while (it != m_banned_subnets.end()) {
            CSubNet sub_net = (*it).first;
            CBanEntry ban_entry = (*it).second;
            if (now > ban_entry.nBanUntil) {
                m_banned_subnets.erase(it++);
                m_is_dirty = true;
                notify_ui = true;
                LogPrint(BCLog::NET, "%s: Removed banned node ip/subnet from banlist.dat: %s\n", __func__, sub_net.ToString());
            } else
                ++it;
        }
        for (auto i = m_banned_addrs.begin(); i != m_banned_addrs.end(); ) {
            CNetAddr addr = i->first;
            CBanEntry ban_entry = i->second;
            if (now > ban_entry.nBanUntil) {
                m_banned_addrs.erase(i++);
                m_is_dirty = true;
                notify_ui = true;
                CSubNet sub_net(addr);
                LogPrint(BCLog::NET, "%s: Removed banned node ip/subnet from banlist.dat: %s\n", __func__, sub_net.ToString());
            } else {
                ++i;
            }
        }
    }
    // update UI
    if (notify_ui && m_client_interface) {
        m_client_interface->BannedListChanged();
    }
}

bool BanMan::BannedSetIsDirty()
{
    LOCK(m_cs_banned);
    return m_is_dirty;
}

void BanMan::SetBannedSetDirty(bool dirty)
{
    LOCK(m_cs_banned);
    m_is_dirty = dirty;
}
