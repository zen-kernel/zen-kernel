.. SPDX-License-Identifier: GPL-2.0

======
futex2
======

:Author: André Almeida <andrealmeid@collabora.com>

futex, or fast user mutex, is a set of syscalls to allow the userspace to create
performant synchronization mechanisms, such as mutexes, semaphores and
conditional variables in userspace. C standard libraries, like glibc, uses it
as means to implements more high level interfaces like pthreads.

The interface
=============

uAPI functions
--------------

.. kernel-doc:: kernel/futex2.c
   :identifiers: sys_futex_wait sys_futex_wake sys_futex_waitv sys_futex_requeue

uAPI structures
---------------

.. kernel-doc:: include/uapi/linux/futex.h

The ``flag`` argument
---------------------

The flag is used to specify the size of the futex word
(FUTEX_[8, 16, 32]). It's mandatory to define one, since there's no
default size.

By default, the timeout uses a monotonic clock, but can be used as a realtime
one by using the FUTEX_REALTIME_CLOCK flag.

By default, futexes are of the private type, that means that this user address
will be accessed by threads that shares the same memory region. This allows for
some internal optimizations, so they are faster. However, if the address needs
to be shared with different processes (like using ``mmap()`` or ``shm()``), they
need to be defined as shared and the flag FUTEX_SHARED_FLAG is used to set that.

By default, the operation has no NUMA-awareness, meaning that the user can't
choose the memory node where the kernel side futex data will be stored. The
user can choose the node where it wants to operate by setting the
FUTEX_NUMA_FLAG and using the following structure (where X can be 8, 16, or
32)::

 struct futexX_numa {
         __uX value;
         __sX hint;
 };

This structure should be passed at the ``void *uaddr`` of futex functions. The
address of the structure will be used to be waited on/waken on, and the
``value`` will be compared to ``val`` as usual. The ``hint`` member is used to
defined which node the futex will use. When waiting, the futex will be
registered on a kernel-side table stored on that node; when waking, the futex
will be searched for on that given table. That means that there's no redundancy
between tables, and the wrong ``hint`` value will led to undesired behavior.
Userspace is responsible for dealing with node migrations issues that may
occur. ``hint`` can range from [0, MAX_NUMA_NODES], for specifying a node, or
-1, to use the same node the current process is using.

When not using FUTEX_NUMA_FLAG on a NUMA system, the futex will be stored on a
global table on some node, defined at compilation time.

The ``timo`` argument
---------------------

As per the Y2038 work done in the kernel, new interfaces shouldn't add timeout
options known to be buggy. Given that, ``timo`` should be a 64bit timeout at
all platforms, using an absolute timeout value.

Implementation
==============

The internal implementation follows a similar design to the original futex.
Given that we want to replicate the same external behavior of current futex,
this should be somewhat expected.

Waiting
-------

For the wait operations, they are all treated as if you want to wait on N
futexes, so the path for futex_wait and futex_waitv is the basically the same.
For both syscalls, the first step is to prepare an internal list for the list
of futexes to wait for (using struct futexv_head). For futex_wait() calls, this
list will have a single object.

We have a hash table, were waiters register themselves before sleeping.  Then,
the wake function checks this table looking for waiters at uaddr.  The hash
bucket to be used is determined by a struct futex_key, that stores information
to uniquely identify an address from a given process. Given the huge address
space, there'll be hash collisions, so we store information to be later used on
collision treatment.

First, for every futex we want to wait on, we check if (``*uaddr == val``).
This check is done holding the bucket lock, so we are correctly serialized with
any futex_wake() calls. If any waiter fails the check above, we dequeue all
futexes. The check (``*uaddr == val``) can fail for two reasons:

- The values are different, and we return -EAGAIN. However, if while
  dequeueing we found that some futex were awakened, we prioritize this
  and return success.

- When trying to access the user address, we do so with page faults
  disabled because we are holding a bucket's spin lock (and can't sleep
  while holding a spin lock). If there's an error, it might be a page
  fault, or an invalid address. We release the lock, dequeue everyone
  (because it's illegal to sleep while there are futexes enqueued, we
  could lose wakeups) and try again with page fault enabled. If we
  succeeded, this means that the address is valid, but we need to do
  all the work again. For serialization reasons, we need to have the
  spin lock when getting the user value. Additionally, for shared
  futexes, we also need to recalculate the hash, since the underlying
  mapping mechanisms could have changed when dealing with page fault.
  If, even with page fault enabled, we can't access the address, it
  means it's an invalid user address, and we return -EFAULT. For this
  case, we prioritize the error, even if some futex were awaken.

If the check is OK, they are enqueued on a linked list in our bucket, and
proceed to the next one. If all waiters succeed, we put the thread to sleep
until a futex_wake() call, timeout expires or we get a signal. After waking up,
we dequeue everyone, and check if some futex was awaken. This dequeue is done by
iteratively walking at each element of struct futex_head list.

All enqueuing/dequeuing operations requires to hold the bucket lock, to avoid
racing while modifying the list.

Waking
------

We get the bucket that's storing the waiters at uaddr, and wake the required
number of waiters, checking for hash collision.

There's an optimization that makes futex_wake() not taking the bucket lock if
there's no one to be wake on that bucket. It checks an atomic counter that each
bucket has, if it says 0, than the syscall exits. In order to this work, the
waiter thread increases it before taking the lock, so the wake thread will
correctly see that there's someone waiting and will continue the path to take
the bucket lock. To get the correct serialization, the waiter issues a memory
barrier after increasing the bucket counter and the waker issues a memory
barrier before checking it.

Requeuing
---------

The requeue path first checks for each struct futex_requeue and their flags.
Then, it will compare the excepted value with the one at uaddr1::uaddr.
Following the same serialization explained at Waking_, we increase the atomic
counter for the bucket of uaddr2 before taking the lock. We need to have both
buckets locks at same time so we don't race with others futexes operations. To
ensure the locks are taken in the same order for all threads (and thus avoiding
deadlocks), every requeue operation takes the "smaller" bucket first, when
comparing both addresses.

If the compare with user value succeeds, we proceed by waking ``nr_wake``
futexes, and then requeuing ``nr_requeue`` from bucket of uaddr1 to the uaddr2.
This consists in a simple list deletion/addition and replacing the old futex key
for the new one.

Futex keys
----------

There are two types of futexes: private and shared ones. The private are futexes
meant to be used by threads that shares the same memory space, are easier to be
uniquely identified an thus can have some performance optimization. The elements
for identifying one are: the start address of the page where the address is,
the address offset within the page and the current->mm pointer.

Now, for uniquely identifying shared futex:

- If the page containing the user address is an anonymous page, we can
  just use the same data used for private futexes (the start address of
  the page, the address offset within the page and the current->mm
  pointer) that will be enough for uniquely identifying such futex. We
  also set one bit at the key to differentiate if a private futex is
  used on the same address (mixing shared and private calls do not
  work).

- If the page is file-backed, current->mm maybe isn't the same one for
  every user of this futex, so we need to use other data: the
  page->index, an UUID for the struct inode and the offset within the
  page.

Note that members of futex_key doesn't have any particular meaning after they
are part of the struct - they are just bytes to identify a futex.  Given that,
we don't need to use a particular name or type that matches the original data,
we only need to care about the bitsize of each component and make both private
and shared fit in the same memory space.

Source code documentation
=========================

.. kernel-doc:: kernel/futex2.c
   :no-identifiers: sys_futex_wait sys_futex_wake sys_futex_waitv sys_futex_requeue
