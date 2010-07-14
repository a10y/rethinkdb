
#ifndef __MIRRORED_CACHE_HPP__
#define __MIRRORED_CACHE_HPP__

#include "event_queue.hpp"
#include "cpu_context.hpp"
#include "concurrency/access.hpp"
#include "concurrency/rwi_lock.hpp"
#include "buffer_cache/callbacks.hpp"

// This cache doesn't actually do any operations itself. Instead, it
// provides a framework that collects all components of the cache
// (memory allocation, page lookup, page replacement, writeback, etc.)
// into a coherent whole. This allows easily experimenting with
// various components of the cache to improve performance.

/* Buffer class. */
template <class config_t>
class buf : public iocallback_t,
            public alloc_mixin_t<tls_small_obj_alloc_accessor<alloc_t>, buf<config_t> >,
            public intrusive_list_node_t<buf<config_t> >
{
public:
    typedef typename config_t::serializer_t serializer_t;
    typedef typename config_t::transaction_t transaction_t;
    typedef typename config_t::cache_t cache_t;
    typedef block_available_callback<config_t> block_available_callback_t;

    cache_t *cache;

#ifndef NDEBUG
    // This field helps catch bugs wherein a block is unloaded even though a callback still points
    // to it.
    int active_callback_count;
#endif
  
private:
    block_id_t block_id;
    void *data;
    
    /* Is data valid, or are we waiting for a read? */
    bool cached;
    
    typedef intrusive_list_t<block_available_callback_t> callbacks_t;
    callbacks_t load_callbacks;
    
public:
    // Each of these local buf types holds a redundant pointer to the buf that they are a part of
    typename config_t::writeback_t::local_buf_t writeback_buf;
    typename config_t::page_repl_t::local_buf_t page_repl_buf;
    typename config_t::concurrency_t::local_buf_t concurrency_buf;
    typename config_t::page_map_t::local_buf_t page_map_buf;

    buf(cache_t *cache, block_id_t block_id);
    ~buf();

    void release();

	// This function is called by the code that loads the data into the block. Other users, which
	// expect the block to already contain valid data, should call ptr().
	void *ptr_possibly_uncached() {
        assert(!safe_to_unload());
    	return data;
    }

    // TODO(NNW) We may want a const version of ptr() as well so the non-const
    // version can verify that the buf is writable; requires pushing const
    // through a bunch of other places (such as array_node) also, however.
    void *ptr() {
    	assert(cached);
        assert(concurrency_buf.lock.locked());
    	return ptr_possibly_uncached();
    }

    block_id_t get_block_id() const { return block_id; }

    void set_cached(bool _cached) { cached = _cached; }
    bool is_cached() const { return cached; }
    
    void set_dirty() { writeback_buf.set_dirty(); }

    // Callback API
    void add_load_callback(block_available_callback_t *callback);

    void notify_on_load();

    virtual void on_io_complete(event_t *event);
	
	bool safe_to_unload();

#ifndef NDEBUG
	// Prints debugging information designed to resolve deadlocks.
	void deadlock_debug();
#endif
};

/* Transaction class. */
template <class config_t>
class transaction : public lock_available_callback_t,
                    public alloc_mixin_t<tls_small_obj_alloc_accessor<alloc_t>, transaction<config_t> >,
                    public sync_callback<config_t>
{
public:
    typedef typename config_t::serializer_t serializer_t;
    typedef typename config_t::concurrency_t concurrency_t;
    typedef typename config_t::cache_t cache_t;
    typedef typename config_t::buf_t buf_t;
    typedef block_available_callback<config_t> block_available_callback_t;
    typedef transaction_begin_callback<config_t> transaction_begin_callback_t;
    typedef transaction_commit_callback<config_t> transaction_commit_callback_t;

    explicit transaction(cache_t *cache, access_t access,
        transaction_begin_callback_t *callback);
    ~transaction();

    cache_t *get_cache() const { return cache; }
    access_t get_access() const { return access; }

    bool commit(transaction_commit_callback_t *callback);

    buf_t *acquire(block_id_t block_id, access_t mode,
                   block_available_callback_t *callback);
    buf_t *allocate(block_id_t *new_block_id);

private:
    virtual void on_lock_available() { begin_callback->on_txn_begin(this); }
    virtual void on_sync();

    cache_t *cache;
    access_t access;
    transaction_begin_callback_t *begin_callback;
    transaction_commit_callback_t *commit_callback;
    enum { state_open, state_committing, state_committed } state;

public:
#ifndef NDEBUG
    event_queue_t *event_queue; // For asserts that we haven't changed CPU.
#endif
};

template <class config_t>
struct mirrored_cache_t : public config_t::serializer_t,
                          public config_t::page_map_t,
                          public config_t::page_repl_t,
                          public config_t::writeback_t,
                          public buffer_alloc_t
{
public:
    typedef typename config_t::serializer_t serializer_t;
    typedef typename config_t::page_repl_t page_repl_t;
    typedef typename config_t::writeback_t writeback_t;
    typedef typename config_t::transaction_t transaction_t;
    typedef typename config_t::concurrency_t concurrency_t;
    typedef typename config_t::page_map_t page_map_t;
    typedef typename config_t::buf_t buf_t;

public:
    // TODO: how do we design communication between cache policies?
    // Should they all have access to the cache, or should they only
    // be given access to each other as necessary? The first is more
    // flexible as anyone can access anyone else, but encourages too
    // many dependencies. The second is more strict, but might not be
    // extensible when some policy implementation requires access to
    // components it wasn't originally given.
    mirrored_cache_t(
            size_t _block_size,
            size_t _max_size,
            bool wait_for_flush,
            unsigned int flush_timer_ms,
            unsigned int flush_threshold_percent) : 
        serializer_t(_block_size),
        page_repl_t(
            // Launch page replacement if the user-specified maximum number of blocks is reached
            _max_size / _block_size,
            this),
        writeback_t(
            this,
            wait_for_flush,
            flush_timer_ms,
            _max_size / _block_size * flush_threshold_percent / 100)
#ifndef NDEBUG
        , n_trans_created(0), n_trans_freed(0),
        n_blocks_acquired(0), n_blocks_released(0)
#endif
        {}
    ~mirrored_cache_t();

    void start() {
    	writeback_t::start();
    }
    void shutdown(sync_callback<config_t> *cb) {
    	writeback_t::shutdown(cb);
    }

    // Transaction API
    transaction_t *begin_transaction(access_t access,
        transaction_begin_callback<config_t> *callback);

    void aio_complete(buf_t *buf, bool written);
    
    buf_t *create_buf(block_id_t id);
    
	/* do_unload_buf unloads a buf from memory, freeing the buf_t object. It should only be called
    on a buf that is not in use and not dirty. It is called by the cache's destructor and by the
    page replacement policy. */
    void do_unload_buf(buf_t *buf);
    
#ifndef NDEBUG
	// Prints debugging information designed to resolve deadlocks
	void deadlock_debug();
#endif

#ifndef NDEBUG
public:
    int n_trans_created, n_trans_freed;
    int n_blocks_acquired, n_blocks_released;
#endif
};

#include "buffer_cache/mirrored.tcc"

#endif // __MIRRORED_CACHE_HPP__

