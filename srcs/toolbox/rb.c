/* 
 * Copyright (c) 2005-2010 by KoanLogic s.r.l.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <toolbox/rb.h>

struct u_rb_s
{
    char *base;     /* base address of the mmap'd region */
    size_t sz;      /* ring buffer size */
    size_t wr_off;  /* write offset */
    size_t rd_off;  /* read offset */
};

static char *write_addr (u_rb_t *rb);
static void write_incr (u_rb_t *rb, size_t cnt);
static char *read_addr (u_rb_t *rb);
static void read_incr (u_rb_t *rb, size_t cnt);
static size_t round_sz (size_t sz);

/**
    \defgroup rb Ring Buffer
    \{
        The \ref rb module provides an efficient implementation of a circular
        buffer.
 */
 
/**
 *  \brief  Create a new ring buffer object
 *
 *  Create a new ::u_rb_t object of size (at least) \p hint_sz and return it 
 *  at \p *prb 
 *
 *  \param  hint_sz the suggested size in bytes for the ring buffer (the actual
 *                  size could be more than that because of alignement needs)
 *  \param  prb     result argument which holds the reference to the newly
 *                  created ::u_rb_t object
 *
 *  \retval  0  on success
 *  \retval -1  on error
 */
int u_rb_create (size_t hint_sz, u_rb_t **prb)
{
    int fd = -1;
    u_rb_t *rb = NULL;
    char path[] = "/tmp/rb-XXXXXX";
    
    dbg_err_sif ((rb = u_zalloc(sizeof(u_rb_t))) == NULL);
    dbg_err_sif ((fd = mkstemp(path)) == -1);
    dbg_err_sif (u_remove(path));
 
    /* round the supplied size to a page multiple (mmap is quite picky
     * about page boundary alignement) */
    rb->sz = round_sz(hint_sz);
    rb->wr_off = 0;
    rb->rd_off = 0;
 
    dbg_err_sif (ftruncate(fd, rb->sz) == -1);

    /* mmap 2 * rb->sz bytes */
    rb->base = mmap(NULL, rb->sz << 1, PROT_NONE, 
            MAP_ANON | MAP_PRIVATE, -1, 0);
    dbg_err_sif (rb->base == MAP_FAILED);
 
    /* first half of the mmap'd region */
    dbg_err_sif (mmap(rb->base, rb->sz, PROT_READ | PROT_WRITE, 
            MAP_FIXED | MAP_SHARED, fd, 0) != rb->base);
 
    /* second half */
    dbg_err_sif (mmap(rb->base + rb->sz, rb->sz, PROT_READ | PROT_WRITE, 
            MAP_FIXED | MAP_SHARED, fd, 0) != rb->base + rb->sz);
 
    /* dispose the file descriptor */
    dbg_err_sif (close(fd) == -1);

    *prb = rb;

    return 0;
err:
    u_rb_free(rb);
    U_CLOSE(fd);
    return -1;
}
 
/**
 *  \brief  Dispose a ring buffer object
 *
 *  Dispose the previously allocated ::u_rb_t object \p rb
 *
 *  \param  rb  reference to the ::u_rb_t object that must be disposed
 *
 *  \return nothing
 */
void u_rb_free (u_rb_t *rb)
{
    nop_return_if (rb == NULL, );

    dbg_return_sif (rb->base && (munmap(rb->base, rb->sz << 1) == -1), );
    u_free(rb);

    return;
}

/**
 *  \brief  Return the size of the ring buffer
 *
 *  Return the real size (as rounded by the ::u_rb_create routine) of the 
 *  supplied ::u_rb_t object \p rb
 *
 *  \param  rb  reference to an already allocated ::u_rb_t object
 *
 *  \return the size in bytes of the ring buffer
 */
size_t u_rb_size (u_rb_t *rb)
{
    return rb->sz;
}

/**
 *  \brief  Write to the ring buffer
 *
 *  Try to write \p b_sz bytes of data from the memory block \p b to the 
 *  ::u_rb_t object \p rb
 *
 *  \param  rb      reference to an already allocated ::u_rb_t object where
 *                  data will be written to
 *  \param  b       reference to the memory block which will be copied-in
 *  \param  b_sz    number of bytes starting from \p b that the caller wants
 *                  to be copied into \p rb
 *
 *  \return the number of bytes actually written to the ring buffer (may be 
 *              less than the requested size)
 */
ssize_t u_rb_write (u_rb_t *rb, const void *b, size_t b_sz)
{
    size_t to_be_written;

    dbg_return_if (rb == NULL, -1);
    dbg_return_if (b == NULL, -1);
    dbg_return_if (b_sz > u_rb_size(rb), -1);

    nop_goto_if (!(to_be_written = U_MIN(u_rb_avail(rb), b_sz)), end);

    memcpy(write_addr(rb), b, to_be_written);
    write_incr(rb, to_be_written);

    /* fall through */
end:
    return to_be_written;
}

/**
 *  \brief  Read from the ring buffer
 *
 *  Try to read \p b_sz bytes of data from the ring buffer \p rb and copy it 
 *  to \p b
 *
 *  \param  rb      reference to an already allocated ::u_rb_t object where 
 *                  data will be read from
 *  \param  b       reference to the memory block where data will be copied
 *                  (must be pre-allocated by the caller and of \p b_sz bytes
 *                  at least)
 *  \param  b_sz    number of bytes that the caller wants to be copied into 
 *                  \p b
 *
 *  \return the number of bytes actually read from the ring buffer (may be 
 *              less than the requested size)
 */
ssize_t u_rb_read (u_rb_t *rb, void *b, size_t b_sz)
{
    size_t to_be_read;

    dbg_return_if (b == NULL, -1);
    dbg_return_if (rb == NULL, -1);
    dbg_return_if (b_sz > u_rb_size(rb), -1);

    /* if there is nothing ready to be read go out immediately */
    nop_goto_if (!(to_be_read = U_MIN(u_rb_ready(rb), b_sz)), end);

    memcpy(b, read_addr(rb), to_be_read);
    read_incr(rb, to_be_read);

    /* fall through */
end:
    return to_be_read;
}

/**
 *  \brief  Reset the ring buffer
 *
 *  Reset read and write offset counters of the supplied ::u_rb_t object \p rb
 *
 *  \param  rb      reference to an already allocated ::u_rb_t object
 *
 *  \retval  0  on success
 *  \retval -1  on failure
 */
int u_rb_clear (u_rb_t *rb)
{
    dbg_return_if (rb == NULL, -1);
    rb->wr_off = rb->rd_off = 0;
    return 0;
}
 
/**
 *  \brief  Return the number of bytes ready to be consumed
 *
 *  Return the number of bytes ready to be consumed for the supplied ::u_rb_t
 *  object \p rb
 *
 *  \param  rb      reference to an already allocated ::u_rb_t object 
 *
 *  \return  the number of bytes ready to be consumed (could be \c 0)
 */
size_t u_rb_ready (u_rb_t *rb)
{
    return rb->wr_off - rb->rd_off;
}
 
/**
 *  \brief  Return the unused buffer space
 *
 *  Return number of unused bytes in the supplied ::u_rb_t object, i.e. 
 *  the dual of ::u_rb_ready.
 *
 *  \param  rb      reference to an already allocated ::u_rb_t object 
 *
 *  \return the number of bytes that can be ::u_rb_write'd before old data 
 *          starts being overwritten
 */
size_t u_rb_avail (u_rb_t *rb)
{
    return rb->sz - u_rb_ready(rb);
}

/**
 *  \}
 */ 
 
/* round requested size to a multiple of PAGESIZE */
static size_t round_sz (size_t sz)
{
    size_t pg_sz = (size_t) sysconf(_SC_PAGE_SIZE);

    return !sz ? pg_sz : (((sz - 1) / pg_sz) + 1) * pg_sz;
}

/* address for write op */
static char *write_addr (u_rb_t *rb)
{
    return rb->base + rb->wr_off;
}
 
/* shift the write pointer */
static void write_incr (u_rb_t *rb, size_t cnt)
{
    rb->wr_off += cnt;
}
 
/* address for next read op */
static char *read_addr (u_rb_t *rb)
{
    return rb->base + rb->rd_off;
}
 
/* shift the read pointer of 'cnt' positions */
static void read_incr (u_rb_t *rb, size_t cnt)
{
    /* 
     * assume 'rb' and 'cnt' sanitized 
     */
    rb->rd_off += cnt;

    /* When the read offset is advanced into the second virtual-memory region, 
     * both offsets - read and write - are decremented by the length of the 
     * underlying buffer */
    if (rb->rd_off >= rb->sz)
    {
        rb->rd_off -= rb->sz;
        rb->wr_off -= rb->sz;
    }

    return;
}
