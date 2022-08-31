#include "sleeper.h"
#include "room.h"
#include "wheat_command.h"
#include <atomic>
#include "logger.h"
#include "violation_detector.h"
#include "traffic_recorder.h"
#include "black_list.h"

namespace wheat
{

Sleeper::Sleeper(Room& room, socket sock)
    : m_room(room)
    , m_id(MakeSleeperId())
    , m_sock(std::move(sock))
    , m_ip(m_sock.remote_endpoint().address().to_v4())
    , m_timer(m_sock.get_executor())
{
    m_timer.expires_at(std::chrono::steady_clock::time_point::max());
    LOG_INFO("new sleeper, sleeper_id:%lld, remote_ip:%s", m_id, GetIp().c_str());
}

//加入房间，启动一个收消息的协程，一个发消息的协程 
void Sleeper::Start()
{
    //记录连接、开始观察 
    IPTrafficRecorder::Instance().OnConnection(m_ip);
    ViolationDetector::Instance().AddObserver("ip", GetIp(),
        [this](std::string reason)
        {
            auto ip_str = GetIp();
            LOG_WARN("OnViolation, sleeper_id:%lld, ip:%s, reason:%s",
                m_id, ip_str.c_str(), reason.c_str());
            //加入黑名单 
            blacklist::BlackList::Instance().AddIpToBlockList(ip_str);

            Stop();
        });

    if (!m_room.Join(m_id, shared_from_this()))
    {
        LOG_INFO("%s, Join failed, so disconnect socket, sleeper_id:%lld, ip:%s", 
            __func__, m_id, GetIp().c_str());
        Stop();
    }
    else
    {
        asio::co_spawn(m_sock.get_executor(),
            [self = shared_from_this()]{ return self->Reader(); },
            asio::detached);

        asio::co_spawn(m_sock.get_executor(),
            [self = shared_from_this()]{ return self->Writer(); },
            asio::detached);
    }
}

std::string Sleeper::GetIp() const
{
    return m_ip.to_string();
}

void Sleeper::Deliver(std::string msg)
{
    m_write_msgs.emplace_back(std::move(msg));
    m_timer.cancel_one();
}

std::string Sleeper::MakeSelfInfo() const
{
    std::string info;
    auto make_cmd_str = [this](const WheatCommand& cmd) { return PackCommandWithId(m_id, cmd); };

    info += make_cmd_str(CmdSleeper{ m_id });
    info += make_cmd_str(CmdName{ m_name });
    info += make_cmd_str(CmdType{ m_sex });

    if (m_bed_id != -1)
    {
        info += make_cmd_str(CmdSleep{ m_bed_id });
    }
    else
    {
        info += make_cmd_str(CmdPos{ m_pos });
    }
    return info;
}

asio::awaitable<void> Sleeper::Reader()
{
    try
    {
        //循环读取数据
        while (true)
        {
            std::string buffer;
            auto n = co_await asio::async_read_until(m_sock,
                asio::dynamic_buffer(buffer, 4096), '\0', asio::use_awaitable);
            IPTrafficRecorder::Instance().OnData(m_ip, n * 8);

            try
            {
                std::string_view msg(buffer.data(), n);
                LOG_DEBUG("%s, sleeper_id:%lld, on commad:%s", __func__, m_id, msg.data());

                //部分消息不需要(或者处理失败时不需要)转发 
                bool forward = true;
                std::visit(
                overloaded{
                    [this, &forward](CmdSleep cmd) {
                        if (m_room.Sleep(m_id, cmd.bed_id))
                            m_bed_id = cmd.bed_id;
                        else
                            forward = false;
                    },
                    [this](CmdGetup cmd) { m_room.GetUp(m_id); },
                    [this](CmdName cmd) { 
                        m_name = std::move(cmd.name); 
                        LOG_INFO("sleeper:%lld's name is:%s", m_id, m_name.c_str());
                    },
                    [this](CmdType cmd) { 
                        m_sex = std::move(cmd.sex); 
                        LOG_INFO("sleeper:%lld's sex is:%d", m_id, m_sex);
                    },
                    [this](CmdChat cmd) { 
                        LOG_INFO("sleeper:%lld say:%s", m_id, cmd.msg.c_str());
                    },
                    [this](CmdPos cmd) { m_pos = cmd.pos; },
                    [this](CmdMove cmd) { m_pos = cmd.pos; },
                    [this](CmdVoteKickStart cmd) { m_room.VoteKickStart(cmd.kick_id); },
                    [this, &forward](CmdVoteAgree cmd) { m_room.Agree(m_id); forward = false; },
                    [this, &forward](CmdVoteRefuse cmd) { m_room.Refuse(m_id); forward = false; },
                    [](auto&&) { }
                }, 
                ParseCommand(msg));

                if (forward)
                    m_room.Deliver(m_id, msg);
            }
            catch (const std::exception& e)
            {
                LOG_WARN("%s, ParseCommand failed, err:%s, sleeper_id:%lld", __func__, e.what(), m_id);
            }
        }
    }
    catch (const std::exception& e)
    {
        Stop();
        LOG_INFO("%s exception, asio read failed, err:%s, sleeper_id:%lld, remote_ip:%s", 
            __func__, e.what(), m_id, GetIp().c_str());
    }
}


asio::awaitable<void> Sleeper::Writer()
{
    try
    {
        while (m_sock.is_open())
        {
            if (m_write_msgs.empty())
            {
                //在定时器处等待，直到发送消息队列有数据 
                asio::error_code ec;
                co_await m_timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            }
            else
            {
                co_await asio::async_write(m_sock,
                    asio::buffer(m_write_msgs.front()), asio::use_awaitable);
                m_write_msgs.pop_front();
            }
        }
    }
    catch (const std::exception& e)
    {
        Stop();
        LOG_WARN("%s exception, asio write failed, err:%s, sleeper_id:%lld, remote_ip:%s", 
            __func__, e.what(), m_id, GetIp().c_str());
    }
}

void Sleeper::Stop()
{
    if (m_is_stoped)
        return;
    m_is_stoped = true;
    LOG_INFO("%s, sleeper_id:%lld, ip:%s", __func__, m_id, GetIp().c_str());
    m_room.Leave(m_id);
    m_sock.close();
    m_timer.cancel();
    //记录连接断开
    IPTrafficRecorder::Instance().OnConnectionClose(m_ip);
}

SleeperId MakeSleeperId()
{
    static std::atomic<SleeperId> global_sleeper_id{ 10000 };
    return global_sleeper_id++;
}

}