.. SPDX-License-Identifier: GPL-2.0

=====================
Multigenerational LRU
=====================

Quick start
===========
Runtime configurations
----------------------
:Required: Write ``1`` to ``/sys/kernel/mm/lru_gen/enable`` if the
 feature wasn't enabled by default.

Recipes
=======
Personal computers
------------------
:Thrashing prevention: Write ``N`` to
 ``/sys/kernel/mm/lru_gen/min_ttl_ms`` to prevent the working set of
 ``N`` milliseconds from getting evicted. The OOM killer is invoked if
 this working set can't be kept in memory. Based on the average human
 detectable lag (~100ms), ``N=1000`` usually eliminates intolerable
 lags due to thrashing. Larger values like ``N=3000`` make lags less
 noticeable at the cost of more OOM kills.

Data centers
------------
:Debugfs interface: ``/sys/kernel/debug/lru_gen`` has the following
 format:
 ::

   memcg  memcg_id  memcg_path
     node  node_id
       min_gen  birth_time  anon_size  file_size
       ...
       max_gen  birth_time  anon_size  file_size

 ``min_gen`` is the oldest generation number and ``max_gen`` is the
 youngest generation number. ``birth_time`` is in milliseconds.
 ``anon_size`` and ``file_size`` are in pages.

 This file also accepts commands in the following subsections.
 Multiple command lines are supported, so does concatenation with
 delimiters ``,`` and ``;``.

 ``/sys/kernel/debug/lru_gen_full`` contains additional stats for
 debugging.

:Working set estimation: Write ``+ memcg_id node_id max_gen
 [can_swap [full_scan]]`` to ``/sys/kernel/debug/lru_gen`` to trigger
 the aging. It scans PTEs for accessed pages and promotes them to the
 youngest generation ``max_gen``. Then it creates a new generation
 ``max_gen+1``. Set ``can_swap`` to 1 to scan for accessed anon pages
 when swap is off. Set ``full_scan`` to 0 to reduce the overhead as
 well as the coverage when scanning PTEs.

:Proactive reclaim: Write ``- memcg_id node_id min_gen [swappiness
 [nr_to_reclaim]]`` to ``/sys/kernel/debug/lru_gen`` to trigger the
 eviction. It evicts generations less than or equal to ``min_gen``.
 ``min_gen`` should be less than ``max_gen-1`` as ``max_gen`` and
 ``max_gen-1`` aren't fully aged and therefore can't be evicted. Use
 ``nr_to_reclaim`` to limit the number of pages to evict.
