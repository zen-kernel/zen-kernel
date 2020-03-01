# SPDX-License-Identifier: GPL-2.0
ifdef CONFIG_FUNCTION_TRACER
CFLAGS_REMOVE_clock.o = $(CC_FLAGS_FTRACE)
endif

# These files are disabled because they produce non-interesting flaky coverage
# that is not a function of syscall inputs. E.g. involuntary context switches.
KCOV_INSTRUMENT := n

ifneq ($(CONFIG_SCHED_OMIT_FRAME_POINTER),y)
# According to Alan Modra <alan@linuxcare.com.au>, the -fno-omit-frame-pointer is
# needed for x86 only.  Why this used to be enabled for all architectures is beyond
# me.  I suspect most platforms don't need this, but until we know that for sure
# I turn this off for IA-64 only.  Andreas Schwab says it's also needed on m68k
# to get a correct value for the wait-channel (WCHAN in ps). --davidm
CFLAGS_core.o := $(PROFILING) -fno-omit-frame-pointer
endif

alt_scheduler = 0
ifdef CONFIG_SCHED_MUQSS
	alt_scheduler = 1
endif
ifdef CONFIG_SCHED_BMQ
	alt_scheduler = 1
endif

ifeq ($(alt_scheduler), 1)
	ifdef CONFIG_SCHED_MUQSS
		obj-y += MuQSS.o clock.o cputime.o
		obj-y += idle.o
		obj-y += wait.o wait_bit.o swait.o completion.o

		obj-$(CONFIG_SMP) += topology.o
	endif
	ifdef CONFIG_SCHED_BMQ
		obj-y += bmq.o
	endif
else
	obj-y += core.o fair.o rt.o deadline.o
	obj-$(CONFIG_SMP) += cpudeadline.o topology.o stop_task.o
	obj-$(CONFIG_SCHED_AUTOGROUP) += autogroup.o
	obj-$(CONFIG_SCHED_DEBUG) += debug.o
endif

ifndef CONFIG_SCHED_MUQSS
	obj-y += loadavg.o clock.o cputime.o idle.o
	obj-y += wait.o wait_bit.o swait.o completion.o
	obj-$(CONFIG_SMP) += cpupri.o pelt.o
	obj-$(CONFIG_CGROUP_CPUACCT) += cpuacct.o
endif

obj-$(CONFIG_SCHEDSTATS) += stats.o
obj-$(CONFIG_CPU_FREQ) += cpufreq.o
obj-$(CONFIG_CPU_FREQ_GOV_SCHEDUTIL) += cpufreq_schedutil.o
obj-$(CONFIG_MEMBARRIER) += membarrier.o
obj-$(CONFIG_CPU_ISOLATION) += isolation.o
obj-$(CONFIG_PSI) += psi.o
