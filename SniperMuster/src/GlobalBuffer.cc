/* Copyright (C) 2018
   Jiaheng Zou <zoujh@ihep.ac.cn> Tao Lin <lintao@ihep.ac.cn>
   Weidong Li <liwd@ihep.ac.cn> Xingtao Huang <huangxt@sdu.edu.cn>
   This file is part of mt.sniper.
 
   mt.sniper is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
 
   mt.sniper is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.
 
   You should have received a copy of the GNU Lesser General Public License
   along with mt.sniper.  If not, see <http://www.gnu.org/licenses/>. */

#include "SniperMuster/GlobalBuffer.h"
#include "SniperKernel/SniperLog.h"
#include "GlobalStream.h"
#include <iostream>

GlobalBuffer* GlobalBuffer::FromStream(const std::string& name)
{
    auto stream = GlobalStream::Get(name);
    return stream->buffer();
}

GlobalBuffer::GlobalBuffer(int capacity, int cordon)
    : m_capacity(capacity),
      m_cordon(cordon)
{
    if ( m_capacity < 10 ) {
        m_capacity = 10;
        LogWarn << "Capacity is too small, set it to 10" << std::endl;
    }
    if ( m_cordon*1.0/m_capacity < 0.3 ) {
        m_cordon = int(m_capacity*0.3);
        LogWarn << "Cordon is too small, set it to " << m_cordon << std::endl;
    }

    m_store = new Elem[m_capacity];

    m_end   = m_store + m_capacity;
    for ( m_begin = m_store; m_begin != m_end; ++m_begin ) {
        m_begin->dptr = nullptr;
        m_begin->stat = -1;
        m_begin->next = m_begin + 1;
    }
    (--m_end)->next = m_store;

    m_begin = m_store;
    m_end   = m_begin;
    m_ref   = m_begin;
}

GlobalBuffer::~GlobalBuffer()
{
    delete[] m_store;
}

void GlobalBuffer::push_back(void* dptr)
{
    std::unique_lock<std::mutex> ltmp(m_mutemp);
    m_slotCond.wait( ltmp,
            [this] { return this->m_end->next->stat == -1; }
            );
    // suppose there is only one thread to fill a global buffer
    ltmp.unlock();

    std::lock_guard<std::mutex> lock(m_mutex0);
    m_end->dptr = dptr;
    m_end->stat = 0;
    if ( dptr != nullptr ) {
        m_end = m_end->next;
        m_dataCond.notify_one();
    }
    else {
        m_dataCond.notify_all();
    }
}

void* GlobalBuffer::pop_front()
{
    void* dptr = nullptr;

    std::unique_lock<std::mutex> lock(m_mutex1);
    m_doneCond.wait( lock,
            [this] { return this->m_begin->stat == 2; }
            );
    // suppose there is only one thread to pop a global buffer
    lock.unlock();

    if ( m_begin->dptr != nullptr ) {
        dptr = m_begin->dptr;
        m_begin->dptr = nullptr;
        m_begin->stat = -1;
        m_begin = m_begin->next;

        if ( this->rough_size() < m_cordon ) { //notify for push_back
            m_slotCond.notify_one();
        }
    }

    return dptr;
}

GlobalBuffer::Elem* GlobalBuffer::next()
{
    Elem* ref = nullptr;

    std::unique_lock<std::mutex> lock(m_mutex0);
    m_dataCond.wait( lock,
            [this] { return this->m_ref->stat == 0; }
            );

    if ( m_ref->dptr != nullptr ) {
        ref = m_ref;
        ref->stat = 1;
        m_ref = m_ref->next;
    }

    return ref;
}

void GlobalBuffer::setDone(GlobalBuffer::Elem* data)
{
    std::lock_guard<std::mutex> lock(m_mutex1);
    data->stat = 2;
    m_doneCond.notify_one();
}

void GlobalBuffer::setOver(int step)
{
    //FIXME: how to free the data memory ?

    if ( step == 1 ) {
        // prevent infinite waiting of the input stream
        std::lock_guard<std::mutex> lock(m_mutemp);
        m_end = m_ref;
        m_end->next->stat = -1;
        m_slotCond.notify_one();
    }

    else if ( step == 2 ) {
        m_ref->dptr = nullptr;
        setDone(m_ref);
    }
}

unsigned int GlobalBuffer::rough_size()
{
    // not accurate in multi-threads context
    int size = m_end - m_begin;
    if ( size < 0 )
        size += m_capacity;
    return (unsigned int)size;
}
