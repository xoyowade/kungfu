/*****************************************************************************
 * Copyright [2017] [taurus.ai]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *****************************************************************************/

#include "PageService.h"
#include "Journal.h"
#include "Page.h"
#include "Timer.h"
#include "Hash.hpp"
#include "PageUtil.h"
#include "Log.h"

#include <sstream>
#include <signal.h>
#include <nlohmann/json.hpp>

USING_YJJ_NAMESPACE

using json = nlohmann::json;

void signal_callback(int signum)
{
    SPDLOG_INFO("PageService Caught signal {}", signum);
    exit(signum);
}

bool PageService::write(string content, byte msg_type, bool is_last, short source)
{
    if (writer.get() == nullptr)
        return false;
    writer->write_frame(content.c_str(), content.length() + 1, source, msg_type, is_last, -1);
    return true;
}

PageService::PageService(const string& _base_dir) : base_dir(_base_dir), memory_message_buffer(nullptr), memory_message_limit(0), memory_msg_file(MEMORY_MSG_FILE) {
    KungfuLog::setup_log("paged");
    KungfuLog::set_log_level(spdlog::level::info);

    SPDLOG_INFO("Page engine base dir {}", get_kungfu_home());

    for (int s = 1; s < 32; s++)
        signal(s, signal_callback);
}

string PageService::get_memory_msg_file()
{
    return memory_msg_file;
}

size_t PageService::get_memory_msg_file_size()
{
    return MEMORY_MSG_FILE_SIZE;
}

void PageService::start()
{
    getNanoTime(); // init NanoTimer before ReqClient, avoid deadlock

    SPDLOG_INFO("Loading page buffer {}", memory_msg_file);
    memory_message_buffer = PageUtil::LoadPageBuffer(memory_msg_file, MEMORY_MSG_FILE_SIZE, true, true);
    memset(memory_message_buffer, 0, MEMORY_MSG_FILE_SIZE);

    SPDLOG_INFO("Creating writer for {}/{}", PAGED_JOURNAL_FOLDER, PAGED_JOURNAL_NAME);
    writer = JournalWriter::create(PAGED_JOURNAL_FOLDER, PAGED_JOURNAL_NAME, "paged", false);
    write("", MSG_TYPE_PAGED_START);

    SPDLOG_INFO("PageService started");
}

void PageService::stop()
{
    /* write paged end in system journal */
    write("", MSG_TYPE_PAGED_END);
    writer.reset();
    SPDLOG_INFO("PageService stopped");
}

void PageService::process_memory_message()
{
    for (int32_t idx = 0; idx < memory_message_limit; idx++)
    {
        PageServiceMessage* msg = GET_MEMORY_MSG(memory_message_buffer, idx);
        if (msg->status == PAGE_REQUESTING)
        {
            SPDLOG_INFO("Request page for id {}/{}", idx, memory_message_limit);
            if (msg->last_page_num > 0 && msg->last_page_num != msg->page_num)
            {
                short curPage = msg->page_num;
                msg->page_num = msg->last_page_num;
                release_page(*msg);
                msg->page_num = curPage;
            }
            msg->status = initiate_page(*msg);
            msg->last_page_num = msg->page_num;
        }
    }
}

int32_t PageService::register_journal(const string& clientName)
{
    int32_t idx = 0;
    for (; idx < MAX_MEMORY_MSG_NUMBER; idx++)
        if (GET_MEMORY_MSG(memory_message_buffer, idx)->status == PAGE_RAW)
            break;

    if (idx >= MAX_MEMORY_MSG_NUMBER)
    {
        SPDLOG_ERROR("idx {} exceeds limit {}", idx, MAX_MEMORY_MSG_NUMBER);
        return idx;
    }
    if (idx >= memory_message_limit)
    {
        memory_message_limit = idx + 1;
    }

    PageServiceMessage* msg = GET_MEMORY_MSG(memory_message_buffer, idx);
    msg->status = PAGE_OCCUPIED;
    msg->last_page_num = 0;
    SPDLOG_INFO("Register journal for {} with id {}", clientName, idx);
    return idx;
}

uint32_t PageService::register_client(const string& clientName, int pid, bool isWriter)
{
    SPDLOG_INFO("Register client {} with isWriter {}", clientName, isWriter);

    map<int, vector<string> >::iterator it = pidClient.find(pid);
    if (it == pidClient.end())
        pidClient[pid] = {clientName};
    else
        pidClient[pid].push_back(clientName);

    std::stringstream ss;
    ss << clientName << getNanoTime() << pid;
    uint32_t hashCode = MurmurHash2(ss.str().c_str(), ss.str().length(), HASH_SEED);
    return hashCode;
}

void PageService::release_page_at(int idx)
{
    PageServiceMessage* msg = GET_MEMORY_MSG(memory_message_buffer, idx);
    if (msg->status == PAGE_ALLOCATED)
        release_page(*msg);
    msg->status = PAGE_RAW;
}

byte PageService::initiate_page(const PageServiceMessage& msg)
{
    SPDLOG_INFO("Initiate page {}/{}", msg.folder, msg.name);

    string path = PageUtil::GenPageFullPath(msg.folder, msg.name, msg.page_num);
    if (fileAddrs.find(path) == fileAddrs.end())
    {
        void* buffer = nullptr;
        if (!PageUtil::FileExists(path))
        {   // this file is not exist....
            if (!msg.is_writer)
                return PAGE_NON_EXIST;
            else
            {
                auto tempPageIter = fileAddrs.find(TEMP_PAGE);
                if (tempPageIter != fileAddrs.end())
                {
                    int ret = rename((TEMP_PAGE).c_str(), path.c_str());
                    if (ret < 0)
                    {
                        SPDLOG_ERROR("Cannot rename from {} to {}", TEMP_PAGE, path);
                        return PAGE_CANNOT_RENAME_FROM_TEMP;
                    }
                    else
                    {
                        SPDLOG_INFO("Renamed {} to {}", TEMP_PAGE, path);
                        buffer = tempPageIter->second;
                        fileAddrs.erase(tempPageIter);
                    }
                }
                if (buffer == nullptr)
                    buffer = PageUtil::LoadPageBuffer(path, JOURNAL_PAGE_SIZE, true, true);
            }
        }
        else
        {   // exist file but not loaded, map and lock immediately.
            buffer = PageUtil::LoadPageBuffer(path, JOURNAL_PAGE_SIZE, false, true);
        }

        SPDLOG_INFO("Added buffer {} to {}", buffer, path);
        fileAddrs[path] = buffer;
    }

    if (msg.is_writer)
    {
        auto count_it = fileWriterCounts.find(msg);
        if (count_it == fileWriterCounts.end())
            fileWriterCounts[msg] = 1;
        else
            return PAGE_MORE_THAN_ONE_WRITE;
    }
    else
    {
        auto count_it = fileReaderCounts.find(msg);
        if (count_it == fileReaderCounts.end())
            fileReaderCounts[msg] = 1;
        else
            count_it->second ++;
    }
    return PAGE_ALLOCATED;
}

void PageService::release_page(const PageServiceMessage& msg)
{
    SPDLOG_INFO("Release page {}/{}", msg.folder, msg.name);

    map<PageServiceMessage, int>::iterator count_it;
    if (msg.is_writer)
    {
        count_it = fileWriterCounts.find(msg);
        if (count_it == fileWriterCounts.end())
        {
            SPDLOG_ERROR("cannot find key at fileWriterCounts in exit_client");
            return;
        }
    }
    else
    {
        count_it = fileReaderCounts.find(msg);
        if (count_it == fileReaderCounts.end())
        {
            SPDLOG_ERROR("cannot find key at fileReaderCounts in exit_client");
            return;
        }
    }
    count_it->second --;
    if (count_it->second == 0)
    {
        bool otherSideEmpty = false;
        if (msg.is_writer)
        {
            fileWriterCounts.erase(count_it);
            otherSideEmpty = fileReaderCounts.find(msg) == fileReaderCounts.end();
        }
        else
        {
            fileReaderCounts.erase(count_it);
            otherSideEmpty = fileWriterCounts.find(msg) == fileWriterCounts.end();
        }
        if (otherSideEmpty)
        {
            string path = PageUtil::GenPageFullPath(msg.folder, msg.name, msg.page_num);
            auto file_it = fileAddrs.find(path);
            if (file_it != fileAddrs.end())
            {
                void* addr = file_it->second;
                SPDLOG_INFO("Release page at {} with address {}", path, addr);
                PageUtil::ReleasePageBuffer(addr, JOURNAL_PAGE_SIZE, true);
                fileAddrs.erase(file_it);
            }
        }
    }
}