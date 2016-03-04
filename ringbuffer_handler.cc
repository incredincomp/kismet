/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#include <stdlib.h>

#include "util.h"
#include "ringbuf2.h"
#include "ringbuffer_handler.h"

RingbufferHandler::RingbufferHandler(size_t r_buffer_sz, size_t w_buffer_sz) {
    if (r_buffer_sz != 0)
        read_buffer = new RingbufV2(r_buffer_sz);
    else
        read_buffer = NULL;

    if (w_buffer_sz != 0)
        write_buffer = new RingbufV2(w_buffer_sz);
    else
        write_buffer = NULL;

    pthread_mutex_init(&handler_locker, NULL);
    pthread_mutex_init(&callback_locker, NULL);
}

RingbufferHandler::~RingbufferHandler() {
    pthread_mutex_lock(&handler_locker);

    if (read_buffer)
        delete read_buffer;

    if (write_buffer)
        delete write_buffer;

    pthread_mutex_unlock(&handler_locker);

    pthread_mutex_destroy(&handler_locker);
    pthread_mutex_destroy(&callback_locker);
}

size_t RingbufferHandler::GetReadBufferSize() {
    local_locker lock(&handler_locker);

    if (read_buffer)
        return read_buffer->size();

    return 0;
}

size_t RingbufferHandler::GetWriteBufferSize() {
    local_locker lock(&handler_locker);

    if (write_buffer)
        return write_buffer->size();

    return 0;
}

size_t RingbufferHandler::GetReadBufferUsed() {
    local_locker lock(&handler_locker);

    if (read_buffer)
        return read_buffer->used();

    return 0;
}

size_t RingbufferHandler::GetWriteBufferUsed() {
    local_locker lock(&handler_locker);

    if (write_buffer)
        return write_buffer->used();

    return 0;
}

size_t RingbufferHandler::GetReadBufferFree() {
    local_locker lock(&handler_locker);

    if (read_buffer)
        return read_buffer->available();

    return 0;
}

size_t RingbufferHandler::GetWriteBufferFree() {
    local_locker lock(&handler_locker);

    if (write_buffer)
        return write_buffer->available();

    return 0;
}

size_t RingbufferHandler::GetReadBufferData(void *in_ptr, size_t in_sz) {
    local_locker lock(&handler_locker);

    if (read_buffer) 
        return read_buffer->read(in_ptr, in_sz);

    return 0;
}

size_t RingbufferHandler::GetWriteBufferData(void *in_ptr, size_t in_sz) {
    local_locker lock(&handler_locker);

    if (write_buffer)
        return write_buffer->read(in_ptr, in_sz);

    return 0;
}

size_t RingbufferHandler::PeekReadBufferData(void *in_ptr, size_t in_sz) {
    local_locker lock(&handler_locker);

    if (read_buffer)
        return read_buffer->peek(in_ptr, in_sz);

    return 0;
}

size_t RingbufferHandler::PeekWriteBufferData(void *in_ptr, size_t in_sz) {
    local_locker lock(&handler_locker);

    if (write_buffer)
        return write_buffer->peek(in_ptr, in_sz);

    return 0;
}

size_t RingbufferHandler::PutReadBufferData(void *in_ptr, size_t in_sz) {
    size_t ret;

    {
        // Sub-context for locking so we don't lock read-op out
        local_locker lock(&handler_locker);

        if (!read_buffer)
            return 0;

        ret = read_buffer->write(in_ptr, in_sz);
    }

    {
        // Lock just the callback handler because the callback
        // needs to interact with us
        local_locker lock(&callback_locker);

        if (rbuf_notify)
            rbuf_notify->ReadBufferFilled(ret);
    }

    return ret;
}
    
size_t RingbufferHandler::PutWriteBufferData(void *in_ptr, size_t in_sz) {
    size_t ret;

    {
        // Sub-context for locking so we don't lock read-op out
        local_locker lock(&handler_locker);

        if (!write_buffer)
            return 0;

        ret = write_buffer->write(in_ptr, in_sz);
    }

    {
        // Lock just the callback handler because the callback
        // needs to interact with us
        local_locker lock(&callback_locker);

        if (wbuf_notify)
            wbuf_notify->WriteBufferFilled(ret);
    }

    return ret;
}

void RingbufferHandler::SetReadBufferInterface(RingbufferInterface *in_interface) {
    local_locker lock(&callback_locker);

    rbuf_notify = in_interface;
}

void RingbufferHandler::SetWriteBufferInterface(RingbufferInterface *in_interface) {
    local_locker lock(&callback_locker);

    wbuf_notify = in_interface;
}

void RingbufferHandler::RemoveReadBufferInterface() {
    local_locker lock(&callback_locker);

    rbuf_notify = NULL;
}

void RingbufferHandler::RemoveWriteBufferInterface() {
    local_locker lock(&callback_locker);

    wbuf_notify = NULL;
}
    

