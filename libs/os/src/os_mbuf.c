/**
 * Copyright (c) Runtime Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Software in this file is based heavily on code written in the FreeBSD source
 * code repostiory.  While the code is written from scratch, it contains 
 * many of the ideas and logic flow in the original source, this is a 
 * derivative work, and the following license applies as well: 
 *
 * Copyright (c) 1982, 1986, 1988, 1991, 1993
 *  The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */


#include "os/os.h"

#include <string.h>

int 
os_mqueue_init(struct os_mqueue *mq, void *arg)
{
    struct os_event *ev;

    STAILQ_INIT(&mq->mq_head);
    
    ev = &mq->mq_ev;
    memset(ev, 0, sizeof(*ev));
    ev->ev_arg = arg;
    ev->ev_type = OS_EVENT_T_MQUEUE_DATA;

    return (0);
}


struct os_mbuf *
os_mqueue_get(struct os_mqueue *mq)
{
    struct os_mbuf_pkthdr *mp;
    struct os_mbuf *m;
    os_sr_t sr;

    OS_ENTER_CRITICAL(sr);
    mp = STAILQ_FIRST(&mq->mq_head);
    if (mp) {
        STAILQ_REMOVE_HEAD(&mq->mq_head, omp_next);
    }
    OS_EXIT_CRITICAL(sr);

    m = OS_MBUF_PKTHDR_TO_MBUF(mp);

    return (m);
}

int 
os_mqueue_put(struct os_mqueue *mq, struct os_eventq *evq, struct os_mbuf *m)
{
    struct os_mbuf_pkthdr *mp;
    os_sr_t sr;
    int rc;

    if (!OS_MBUF_IS_PKTHDR(m)) {
        rc = OS_EINVAL;
        goto err;
    }

    mp = OS_MBUF_PKTHDR(m);

    OS_ENTER_CRITICAL(sr);
    STAILQ_INSERT_TAIL(&mq->mq_head, mp, omp_next);
    OS_EXIT_CRITICAL(sr);

    /* Only post an event to the queue if its specified */
    if (evq) {
        os_eventq_put(evq, &mq->mq_ev);
    }

    return (0);
err:
    return (rc);
}


/**
 * Initialize a pool of mbufs. 
 * 
 * @param omp     The mbuf pool to initialize 
 * @param mp      The memory pool that will hold this mbuf pool 
 * @param buf_len The length of the buffer itself. 
 * @param nbufs   The number of buffers in the pool 
 *
 * @return 0 on success, error code on failure. 
 */
int 
os_mbuf_pool_init(struct os_mbuf_pool *omp, struct os_mempool *mp, uint16_t buf_len, 
        uint16_t nbufs)
{
    omp->omp_databuf_len = buf_len - sizeof(struct os_mbuf);
    omp->omp_mbuf_count = nbufs;
    omp->omp_pool = mp;

    return (0);
}

/** 
 * Get an mbuf from the mbuf pool.  The mbuf is allocated, and initialized
 * prior to being returned.
 *
 * @param omp The mbuf pool to return the packet from 
 * @param leadingspace The amount of leadingspace to put before the data 
 *     section by default.
 *
 * @return An initialized mbuf on success, and NULL on failure.
 */
struct os_mbuf * 
os_mbuf_get(struct os_mbuf_pool *omp, uint16_t leadingspace)
{
    struct os_mbuf *om;

    om = os_memblock_get(omp->omp_pool);
    if (!om) {
        goto err;
    }

    SLIST_NEXT(om, om_next) = NULL;
    om->om_flags = 0;
    om->om_len = 0;
    om->om_data = (&om->om_databuf[0] + leadingspace);
    om->om_omp = omp;

    return (om);
err:
    return (NULL);
}

/* Allocate a new packet header mbuf out of the os_mbuf_pool */ 
struct os_mbuf *
os_mbuf_get_pkthdr(struct os_mbuf_pool *omp, uint8_t pkthdr_len)
{
    struct os_mbuf_pkthdr *pkthdr;
    struct os_mbuf *om;

    om = os_mbuf_get(omp, 0);
    if (om) {
        om->om_pkthdr_len = pkthdr_len;
        om->om_data += pkthdr_len + sizeof(struct os_mbuf_pkthdr);

        pkthdr = OS_MBUF_PKTHDR(om);
        pkthdr->omp_len = 0;
        pkthdr->omp_flags = 0;
        STAILQ_NEXT(pkthdr, omp_next) = NULL;
    }

    return om;
}

/**
 * Release a mbuf back to the pool
 *
 * @param omp The Mbuf pool to release back to 
 * @param om  The Mbuf to release back to the pool 
 *
 * @return 0 on success, -1 on failure 
 */
int 
os_mbuf_free(struct os_mbuf *om) 
{
    int rc;

    rc = os_memblock_put(om->om_omp->omp_pool, om);
    if (rc != 0) {
        goto err;
    }

    return (0);
err:
    return (rc);
}

/**
 * Free a chain of mbufs
 *
 * @param omp The mbuf pool to free the chain of mbufs into
 * @param om  The starting mbuf of the chain to free back into the pool 
 *
 * @return 0 on success, -1 on failure 
 */
int 
os_mbuf_free_chain(struct os_mbuf *om)
{
    struct os_mbuf *next;
    int rc;

    while (om != NULL) {
        next = SLIST_NEXT(om, om_next);

        rc = os_mbuf_free(om);
        if (rc != 0) {
            goto err;
        }

        om = next;
    }

    return (0);
err:
    return (rc);
}

/** 
 * Copy a packet header from one mbuf to another.
 *
 * @param omp The mbuf pool associated with these buffers
 * @param new_buf The new buffer to copy the packet header into 
 * @param old_buf The old buffer to copy the packet header from
 */
static inline void 
_os_mbuf_copypkthdr(struct os_mbuf *new_buf, struct os_mbuf *old_buf)
{
    memcpy(&new_buf->om_databuf[0], &old_buf->om_databuf[0], 
            sizeof(struct os_mbuf_pkthdr) + old_buf->om_pkthdr_len);
}

/** 
 * Append data onto a mbuf 
 *
 * @param omp  The mbuf pool this mbuf was allocated out of 
 * @param om   The mbuf to append the data onto 
 * @param data The data to append onto the mbuf 
 * @param len  The length of the data to append 
 * 
 * @return 0 on success, and an error code on failure 
 */
int 
os_mbuf_append(struct os_mbuf *om, const void *data,  uint16_t len)
{
    struct os_mbuf_pool *omp;
    struct os_mbuf *last; 
    struct os_mbuf *new;
    int remainder;
    int space;
    int rc;

    if (om == NULL) {
        rc = OS_EINVAL;
        goto err;
    }

    omp = om->om_omp;

    /* Scroll to last mbuf in the chain */
    last = om;
    while (SLIST_NEXT(last, om_next) != NULL) {
        last = SLIST_NEXT(last, om_next);
    }

    remainder = len;
    space = OS_MBUF_TRAILINGSPACE(last);

    /* If room in current mbuf, copy the first part of the data into the 
     * remaining space in that mbuf.
     */
    if (space > 0) {
        if (space > remainder) {
            space = remainder;
        }

        memcpy(OS_MBUF_DATA(last, void *) + last->om_len , data, space);

        last->om_len += space;
        data += space;
        remainder -= space;
    }

    /* Take the remaining data, and keep allocating new mbufs and copying 
     * data into it, until data is exhausted.
     */
    while (remainder > 0) {
        new = os_mbuf_get(omp, 0); 
        if (!new) {
            break;
        }

        new->om_len = min(omp->omp_databuf_len, remainder);
        memcpy(OS_MBUF_DATA(om, void *), data, new->om_len);
        data += new->om_len;
        remainder -= new->om_len;
        SLIST_NEXT(last, om_next) = new;
        last = new;
    }

    /* Adjust the packet header length in the buffer */
    if (OS_MBUF_IS_PKTHDR(om)) {
        OS_MBUF_PKTHDR(om)->omp_len += len - remainder;
    }

    if (remainder != 0) {
        rc = OS_ENOMEM;
        goto err;
    } 


    return (0);
err:
    return (rc); 
}


/**
 * Duplicate a chain of mbufs.  Return the start of the duplicated chain.
 *
 * @param omp The mbuf pool to duplicate out of 
 * @param om  The mbuf chain to duplicate 
 *
 * @return A pointer to the new chain of mbufs 
 */
struct os_mbuf *
os_mbuf_dup(struct os_mbuf *om)
{
    struct os_mbuf_pool *omp;
    struct os_mbuf *head;
    struct os_mbuf *copy; 

    omp = om->om_omp;

    head = NULL;
    copy = NULL;

    for (; om != NULL; om = SLIST_NEXT(om, om_next)) {
        if (head) {
            SLIST_NEXT(copy, om_next) = os_mbuf_get(omp, 
                    OS_MBUF_LEADINGSPACE(om)); 
            if (!SLIST_NEXT(copy, om_next)) {
                os_mbuf_free_chain(head);
                goto err;
            }

            copy = SLIST_NEXT(copy, om_next);
        } else {
            head = os_mbuf_get(omp, OS_MBUF_LEADINGSPACE(om));
            if (!head) {
                goto err;
            }

            if (OS_MBUF_IS_PKTHDR(om)) {
                _os_mbuf_copypkthdr(head, om);
            }
            copy = head;
        }
        copy->om_flags = om->om_flags;
        copy->om_len = om->om_len;
        memcpy(OS_MBUF_DATA(copy, uint8_t *), OS_MBUF_DATA(om, uint8_t *),
                om->om_len);
    }

    return (head);
err:
    return (NULL);
}

/**
 * Locates the specified absolute offset within an mbuf chain.  The offset
 * can be one past than the total length of the chain, but no greater.
 *
 * @param om                    The start of the mbuf chain to seek within.
 * @param off                   The absolute address to find.
 * @param out_off               On success, this points to the relative offset
 *                                  within the returned mbuf.
 *
 * @return                      The mbuf containing the specified offset on
 *                                  success.
 *                              NULL if the specified offset is out of bounds.
 */
struct os_mbuf *
os_mbuf_off(struct os_mbuf *om, int off, int *out_off)
{
    struct os_mbuf *next;

    while (1) {
        if (om == NULL) {
            return NULL;
        }

        next = SLIST_NEXT(om, om_next);

        if (om->om_len > off ||
            (om->om_len == off && next == NULL)) {

            *out_off = off;
            return om;
        }

        off -= om->om_len;
        om = next;
    }
}

/*
 * Copy data from an mbuf chain starting "off" bytes from the beginning,
 * continuing for "len" bytes, into the indicated buffer.
 *
 * @return                      0 on success;
 *                              -1 if the mbuf does not contain enough data.
 */
int
os_mbuf_copydata(const struct os_mbuf *m, int off, int len, void *dst)
{
    unsigned int count;
    uint8_t *udst;

    udst = dst;

    while (off > 0) {
        if (!m) {
            return (-1);
        }

        if (off < m->om_len)
            break;
        off -= m->om_len;
        m = SLIST_NEXT(m, om_next);
    }
    while (len > 0 && m != NULL) {
        count = min(m->om_len - off, len);
        memcpy(udst, m->om_data + off, count);
        len -= count;
        udst += count;
        off = 0;
        m = SLIST_NEXT(m, om_next);
    }

    return (len > 0 ? -1 : 0);
}

void
os_mbuf_adj(struct os_mbuf *mp, int req_len)
{
    int len = req_len;
    struct os_mbuf *m;
    int count;

    if ((m = mp) == NULL)
        return;
    if (len >= 0) {
        /*
         * Trim from head.
         */
        while (m != NULL && len > 0) {
            if (m->om_len <= len) {
                len -= m->om_len;
                m->om_len = 0;
                m = SLIST_NEXT(m, om_next);
            } else {
                m->om_len -= len;
                m->om_data += len;
                len = 0;
            }
        }
        if (OS_MBUF_IS_PKTHDR(mp))
            OS_MBUF_PKTHDR(mp)->omp_len -= (req_len - len);
    } else {
        /*
         * Trim from tail.  Scan the mbuf chain,
         * calculating its length and finding the last mbuf.
         * If the adjustment only affects this mbuf, then just
         * adjust and return.  Otherwise, rescan and truncate
         * after the remaining size.
         */
        len = -len;
        count = 0;
        for (;;) {
            count += m->om_len;
            if (SLIST_NEXT(m, om_next) == (struct os_mbuf *)0)
                break;
            m = SLIST_NEXT(m, om_next);
        }
        if (m->om_len >= len) {
            m->om_len -= len;
            if (OS_MBUF_IS_PKTHDR(mp))
                OS_MBUF_PKTHDR(mp)->omp_len -= len;
            return;
        }
        count -= len;
        if (count < 0)
            count = 0;
        /*
         * Correct length for chain is "count".
         * Find the mbuf with last data, adjust its length,
         * and toss data from remaining mbufs on chain.
         */
        m = mp;
        if (OS_MBUF_IS_PKTHDR(m))
            OS_MBUF_PKTHDR(m)->omp_len = count;
        for (; m; m = SLIST_NEXT(m, om_next)) {
            if (m->om_len >= count) {
                m->om_len = count;
                if (SLIST_NEXT(m, om_next) != NULL) {
                    os_mbuf_free_chain(SLIST_NEXT(m, om_next));
                    SLIST_NEXT(m, om_next) = NULL;
                }
                break;
            }
            count -= m->om_len;
        }
    }
}

/**
 * Performs a memory compare of the specified region of an mbuf chain against a
 * flat buffer.
 *
 * @param om                    The start of the mbuf chain to compare.
 * @param off                   The offset within the mbuf chain to start the
 *                                  comparison.
 * @param data                  The flat buffer to compare.
 * @param len                   The length of the flat buffer.
 *
 * @return                      0 if both memory regions are identical;
 *                              A memcmp return code if there is a mismatch;
 *                              -1 if the mbuf is too short.
 */
int
os_mbuf_memcmp(const struct os_mbuf *om, int off, const void *data, int len)
{
    int chunk_sz;
    int data_off;
    int om_off;
    int rc;

    if (len <= 0) {
        return 0;
    }

    data_off = 0;
    om = os_mbuf_off((struct os_mbuf *)om, off, &om_off);
    while (1) {
        if (om == NULL) {
            return -1;
        }

        chunk_sz = min(om->om_len - om_off, len - data_off);
        if (chunk_sz > 0) {
            rc = memcmp(om->om_data + om_off, data + data_off, chunk_sz);
            if (rc != 0) {
                return rc;
            }
        }

        data_off += chunk_sz;
        if (data_off == len) {
            return 0;
        }

        om = SLIST_NEXT(om, om_next);
        om_off = 0;

        if (om == NULL) {
            return -1;
        }
    }
}

/**
 * Increases the length of an mbuf chain by adding data to the front.  If there
 * is insufficient room in the leading mbuf, additional mbufs are allocated and
 * prepended as necessary.  If this function fails to allocate an mbuf, the
 * entire chain is freed.
 *
 * The specified mbuf chain does not need to contain a packet header.
 *
 * @param omp                   The mbuf pool to allocate from.
 * @param om                    The head of the mbuf chain.
 * @param len                   The number of bytes to prepend.
 *
 * @return                      The new head of the chain on success;
 *                              NULL on failure.
 */
struct os_mbuf *
os_mbuf_prepend(struct os_mbuf *om, int len)
{
    struct os_mbuf *p;
    int leading;

    while (1) {
        /* Fill the available space at the front of the head of the chain, as
         * needed.
         */
        leading = min(len, OS_MBUF_LEADINGSPACE(om));

        om->om_data -= leading;
        om->om_len += leading;
        if (OS_MBUF_IS_PKTHDR(om)) {
            OS_MBUF_PKTHDR(om)->omp_len += leading;
        }

        len -= leading;
        if (len == 0) {
            break;
        }

        /* The current head didn't have enough space; allocate a new head. */
        if (OS_MBUF_IS_PKTHDR(om)) {
            p = os_mbuf_get_pkthdr(om->om_omp, om->om_pkthdr_len);
        } else {
            p = os_mbuf_get(om->om_omp, 0);
        }
        if (p == NULL) {
            os_mbuf_free_chain(om);
            om = NULL;
            break;
        }

        if (OS_MBUF_IS_PKTHDR(om)) {
            _os_mbuf_copypkthdr(p, om);
            om->om_pkthdr_len = 0;
        }

        /* Move the new head's data pointer to the end so that data can be
         * prepended.
         */
        p->om_data += OS_MBUF_TRAILINGSPACE(p);

        SLIST_NEXT(p, om_next) = om;
        om = p;
    }

    return om;
}

/**
 * Copies the contents of a flat buffer into an mbuf chain, starting at the
 * specified destination offset.  If the mbuf is too small for the source data,
 * it is extended as necessary.  If the destination mbuf contains a packet
 * header, the header length is updated.
 *
 * @param omp                   The mbuf pool to allocate from.
 * @param om                    The mbuf chain to copy into.
 * @param off                   The offset within the chain to copy to.
 * @param src                   The source buffer to copy from.
 * @param len                   The number of bytes to copy.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
os_mbuf_copyinto(struct os_mbuf *om, int off, const void *src, int len)
{
    struct os_mbuf *next;
    struct os_mbuf *cur;
    const uint8_t *sptr;
    int copylen;
    int cur_off;
    int rc;

    /* Find the mbuf,offset pair for the start of the destination. */
    cur = os_mbuf_off(om, off, &cur_off);
    if (cur == NULL) {
        return -1;
    }

    /* Overwrite existing data until we reach the end of the chain. */
    sptr = src;
    while (1) {
        copylen = min(cur->om_len - cur_off, len);
        if (copylen > 0) {
            memcpy(cur->om_data + cur_off, sptr, copylen);
            sptr += copylen;
            len -= copylen;

            copylen = 0;
        }

        if (len == 0) {
            /* All the source data fit in the existing mbuf chain. */
            return 0;
        }

        next = SLIST_NEXT(cur, om_next);
        if (next == NULL) {
            break;
        }

        cur = next;
    }

    /* Append the remaining data to the end of the chain. */
    rc = os_mbuf_append(cur, sptr, len);
    if (rc != 0) {
        return rc;
    }

    /* Fix up the packet header, if one is present. */
    if (OS_MBUF_IS_PKTHDR(om)) {
        OS_MBUF_PKTHDR(om)->omp_len =
            max(OS_MBUF_PKTHDR(om)->omp_len, off + len);
    }

    return 0;
}

/**
 * Attaches a second mbuf chain onto the end of the first.  If the first chain
 * contains a packet header, the header's length is updated.  If the second
 * chain has a packet header, its header is cleared.
 *
 * @param first                 The mbuf chain being attached to.
 * @param second                The mbuf chain that gets attached.
 */
void
os_mbuf_splice(struct os_mbuf *first, struct os_mbuf *second)
{
    struct os_mbuf *next;
    struct os_mbuf *cur;

    /* Point 'cur' to the last buffer in the first chain. */
    cur = first;
    while (1) {
        next = SLIST_NEXT(cur, om_next);
        if (next == NULL) {
            break;
        }

        cur = next;
    }

    /* Attach the second chain to the end of the first. */
    SLIST_NEXT(cur, om_next) = second;

    /* If the first chain has a packet header, calculate the length of the
     * second chain and add it to the header length.
     */
    if (OS_MBUF_IS_PKTHDR(first)) {
        if (OS_MBUF_IS_PKTHDR(second)) {
            OS_MBUF_PKTHDR(first)->omp_len += OS_MBUF_PKTHDR(second)->omp_len;
        } else {
            for (cur = second; cur != NULL; cur = SLIST_NEXT(cur, om_next)) {
                OS_MBUF_PKTHDR(first)->omp_len += cur->om_len;
            }
        }
    }

    second->om_pkthdr_len = 0;
}

#if 0

/**
 * Rearrange a mbuf chain so that len bytes are contiguous, 
 * and in the data area of an mbuf (so that OS_MBUF_DATA() will 
 * work on a structure of size len.)  Returns the resulting 
 * mbuf chain on success, free's it and returns NULL on failure.
 *
 * If there is room, it will add up to "max_protohdr - len" 
 * extra bytes to the contiguous region, in an attempt to avoid being
 * called next time.
 *
 * @param omp The mbuf pool to take the mbufs out of 
 * @param om The mbuf chain to make contiguous
 * @param len The number of bytes in the chain to make contiguous
 *
 * @return The contiguous mbuf chain on success, NULL on failure.
 */
struct os_mbuf *
os_mbuf_pullup(struct os_mbuf *om, uint16_t len)
{
    struct os_mbuf_pool *omp;
    struct os_mbuf *newm;

    omp = om->om_omp;

    if (len > omp->omp_databuf_len) {
        goto err;
    }

    /* Is 'n' bytes already contiguous? */
    if (((uint8_t *) &om->om_databuf[0] + omp->omp_databuf_len) - 
            OS_MBUF_DATA(om, uint8_t *) >= len) {
        newm = om;
        goto done;
    }

    /* Nope, OK. Allocate a new buffer, and then go through and copy 'n' 
     * bytes into that buffer.
     */
    newm = os_mbuf_get(omp, 0);
    if (!newm) {
        goto err;
    }

    written = 0; 
    while (written < len


done:
    return (newm);
err:
    if (om) {
        os_mbuf_free_chain(om);
    }

    return (NULL);
}
#endif
