.. _Coherence:

=====================================
Cache Coherence (MESI)
=====================================

ChampSim models a MESI cache coherence protocol across the caches that are private to each core.
Every cached block carries a MESI state, and the cache that is shared by all cores (in the default configuration, the LLC) keeps a directory that records which cores hold a copy of each block and in what state.

Coherence changes *what* is cached and *when* writebacks happen. It adds no traffic to the timing model: invalidations, downgrades, and eviction notifications are instantaneous.

-------------------
States
-------------------

.. doxygenenum:: champsim::coherence_state

Every ``champsim::cache_block`` has two coherence fields:

``state``
  The MESI state of this copy.

  In a private cache this is the state of the copy held there: ``EXCLUSIVE`` after a read that found no other holder, ``SHARED`` after a read that found another holder (or after another core read the block), and ``MODIFIED`` after a write.

  In the coherence point, ``state`` is the directory's view of the copies held *above* it: ``INVALID`` when no core holds the block, ``EXCLUSIVE`` or ``MODIFIED`` when exactly one core does, and ``SHARED`` when more than one core does.

``sharers``
  In the coherence point, one bit per core that holds a copy above it. Zero in every other cache.

Both fields are visible to replacement policies through the ``current_set`` argument of ``find_victim()`` and through ``intern_->block``.

-------------------
Protocol
-------------------

The coherence point services every request that reaches it from above, whether it hits or misses, and consults its directory before responding.

Read (``LOAD``, ``PREFETCH``, ``TRANSLATION``)
  If no other core holds the block, the requester is granted ``EXCLUSIVE``.
  Otherwise every other holder is downgraded to ``SHARED`` and the requester is granted ``SHARED``.
  If a downgraded copy was dirty, the coherence point's copy becomes dirty so that the eventual writeback to DRAM is preserved.

Read for ownership (``RFO``)
  A store miss in a private cache is forwarded as an ``RFO``.
  Every other holder is invalidated and the requester is granted ``MODIFIED``.

Write hit in a private cache
  An ``EXCLUSIVE`` copy becomes ``MODIFIED`` silently.
  A ``SHARED`` copy requests an upgrade from the directory, which invalidates every other core's copy and promotes every copy the requesting core holds (for example, both its L1D and L2C) to ``MODIFIED``.

Eviction from a private cache
  The directory is notified and recomputes the sharer set for the block.
  A core is removed only when none of its private caches hold the block any more.
  A lone remaining sharer keeps its ``SHARED`` state; MESI has no silent promotion back to ``EXCLUSIVE``.

Writeback arriving at the coherence point
  Treated as an eviction notification for the writing core.

Store landing without ownership
  A store can merge into an MSHR entry whose read was already sent down, so the grant comes back ``EXCLUSIVE`` or ``SHARED``; or another core can read the block while the store's fill is in flight.
  In both cases the store obtains ownership from the directory when its fill lands, exactly as a write hit on a ``SHARED`` copy would.

Invalidation racing a fill
  If a copy is invalidated after it has been granted but before it lands in the cache, the fill is completed (the core receives its response and the miss latency is recorded) but no block is installed.
  The response carries no grant, so caches above it drop their copies as well.

The grant travels in the ``granted`` field of ``champsim::channel::response``.
A response from a source that does not participate in coherence (DRAM) carries no grant; the coherence point ignores it, since the directory decides what to grant.

-------------------
Topology
-------------------

Coherence domains are discovered automatically from the configured hierarchy before the simulation starts, so no configuration keys are needed.

Starting from each core's L1I and L1D channels, the resolver walks downward through ``lower_level`` links.
The last cache on each path becomes the coherence point and owns a ``champsim::coherence::directory``; every other cache on the path becomes a member of that directory on behalf of the core.
A cache on several cores' paths (a level-two cache shared by a cluster, for example) is a member for each of them; an invalidation aimed at one of those cores also removes the copy the other core was using, which is conservative but safe.

Caches that are not reached by this walk, such as TLBs, are outside every coherence domain.
They behave exactly as before, and their blocks report ``EXCLUSIVE`` when clean and ``MODIFIED`` when dirty.

In a single-core configuration the protocol still runs: every private copy is ``EXCLUSIVE`` or ``MODIFIED`` and the directory never records more than one sharer.

-------------------------------------
Sharing between cores
-------------------------------------

By default each core is its own address space: the virtual memory model keys its page mapping on ``(core, virtual page)``, so the same virtual address on two cores maps to two different physical pages.
This models independent processes, which is what multi-programmed trace workloads are, and it means no block is ever held by more than one core.
Coherence still runs, but there is nothing to invalidate or downgrade.

To model threads of one program that share memory, place every core in one address space::

  {
    "num_cores": 2,
    "virtual_memory": { "shared_address_space": true }
  }

Now the same virtual address on any core is the same block, and running the same trace on two cores produces reads that find another holder (``SHARED`` grants and downgrades) and writes that conflict (invalidations).

-------------------
Statistics
-------------------

The coherence point prints a summary at the end of the simulation::

  LLC COHERENCE DIRECTORY  cores: 4  tracked blocks: 12345
  LLC COHERENCE GRANTS  E: ...  S: ...  M: ...  UPGRADES: ...
  LLC COHERENCE INVALIDATIONS: ...  DOWNGRADES: ...  DIRTY ABSORBED: ...  RELEASES: ...  DROPPED FILLS: ...

.. doxygenstruct:: champsim::coherence::directory_stats
   :members:

-------------------
Modeling notes
-------------------

The directory is deliberately idealized:

* It is exact and unbounded; it is not a hardware structure with a size budget.
* Its actions are zero-latency direct calls into the affected caches, so they do not consume queue slots or bandwidth and cannot deadlock.
* The coherence point need not be inclusive. Evicting a block from it leaves the upper-level copies and the directory entry intact; if the block is requested again it is refetched from DRAM and the directory is consulted as usual.
* A replacement policy that bypasses a fill at the coherence point (returns ``NUM_WAY`` from ``find_victim``) while another core's dirty copy is being downgraded has no block to absorb the dirtiness; the writeback obligation is lost in that corner case.

-------------------
API
-------------------

.. doxygenclass:: champsim::coherence::directory
   :members:

.. doxygenfunction:: champsim::coherence::resolve(const std::vector<CACHE*>&, const std::vector<entry_point>&)
