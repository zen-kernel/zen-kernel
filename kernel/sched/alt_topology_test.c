// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

/* This file is included by alt_topology.c so it can exercise static helpers. */

struct sched_topology_test_case {
	const char *name;
	unsigned long active;
	unsigned long pcore;
	unsigned long ecore;
	unsigned long unknown;
	unsigned long smt;
	enum sched_idle_select_mode mode;
	bool heterogeneous;
	bool mixed_cpu_types;
	bool unsupported_ecore_smt;
};

struct sched_topology_test_masks {
	cpumask_t active;
	cpumask_t pcore;
	cpumask_t ecore;
	cpumask_t unknown;
	cpumask_t smt;
};

static void sched_topology_test_mask(cpumask_t *mask, unsigned long bits)
{
	unsigned int cpu;

	cpumask_clear(mask);
	for_each_set_bit(cpu, &bits,
			 min_t(unsigned int, BITS_PER_LONG, nr_cpu_ids))
		cpumask_set_cpu(cpu, mask);
}

static void sched_topology_policy_test(struct kunit *test)
{
	static const struct sched_topology_test_case cases[] = {
		{
			.name = "uniformly unknown",
			.active = BIT(0) | BIT(1),
			.unknown = BIT(0) | BIT(1),
			.mode = SCHED_IDLE_SELECT_GENERIC,
		},
		{
			.name = "performance only",
			.active = BIT(0) | BIT(1),
			.pcore = BIT(0) | BIT(1),
			.mode = SCHED_IDLE_SELECT_GENERIC,
		},
		{
			.name = "efficiency only",
			.active = BIT(0) | BIT(1),
			.ecore = BIT(0) | BIT(1),
			.mode = SCHED_IDLE_SELECT_GENERIC,
		},
		{
			.name = "hybrid without SMT",
			.active = BIT(0) | BIT(1),
			.pcore = BIT(0),
			.ecore = BIT(1),
			.mode = SCHED_IDLE_SELECT_HYBRID,
			.heterogeneous = true,
		},
		{
			.name = "hybrid with P-core SMT",
			.active = BIT(0) | BIT(1) | BIT(2),
			.pcore = BIT(0) | BIT(1),
			.ecore = BIT(2),
			.smt = BIT(0) | BIT(1),
			.mode = SCHED_IDLE_SELECT_HYBRID,
			.heterogeneous = true,
		},
		{
			.name = "hybrid with partial E-core SMT",
			.active = BIT(0) | BIT(1) | BIT(2),
			.pcore = BIT(0),
			.ecore = BIT(1) | BIT(2),
			.smt = BIT(1) | BIT(2),
			.mode = SCHED_IDLE_SELECT_GENERIC,
			.unsupported_ecore_smt = true,
		},
		{
			.name = "hybrid with universal SMT",
			.active = BIT(0) | BIT(1) | BIT(2) | BIT(3),
			.pcore = BIT(0) | BIT(1),
			.ecore = BIT(2) | BIT(3),
			.smt = BIT(0) | BIT(1) | BIT(2) | BIT(3),
			.mode = SCHED_IDLE_SELECT_SMT,
			.unsupported_ecore_smt = true,
		},
		{
			.name = "homogeneous SMT",
			.active = BIT(0) | BIT(1),
			.unknown = BIT(0) | BIT(1),
			.smt = BIT(0) | BIT(1),
			.mode = SCHED_IDLE_SELECT_SMT,
		},
		{
			.name = "mixed known and unknown",
			.active = BIT(0) | BIT(1) | BIT(2),
			.pcore = BIT(0),
			.ecore = BIT(1),
			.unknown = BIT(2),
			.mode = SCHED_IDLE_SELECT_GENERIC,
			.mixed_cpu_types = true,
		},
		{
			.name = "mixed topology with E-core SMT",
			.active = BIT(0) | BIT(1) | BIT(2) | BIT(3),
			.pcore = BIT(0),
			.ecore = BIT(1) | BIT(2),
			.unknown = BIT(3),
			.smt = BIT(1) | BIT(2),
			.mode = SCHED_IDLE_SELECT_GENERIC,
			.mixed_cpu_types = true,
			.unsupported_ecore_smt = true,
		},
	};
	struct sched_topology_test_masks *masks;
	struct sched_topology_policy policy;
	int i;

	if (nr_cpu_ids < 4) {
		kunit_skip(test, "at least four CPU bits are required");
		return;
	}

	masks = kunit_kzalloc(test, sizeof(*masks), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, masks);

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct sched_topology_test_case *casep = &cases[i];

		sched_topology_test_mask(&masks->active, casep->active);
		sched_topology_test_mask(&masks->pcore, casep->pcore);
		sched_topology_test_mask(&masks->ecore, casep->ecore);
		sched_topology_test_mask(&masks->unknown, casep->unknown);
		sched_topology_test_mask(&masks->smt, casep->smt);

		sched_select_topology_policy(&masks->active, &masks->pcore,
					     &masks->ecore, &masks->unknown,
					     &masks->smt, &policy);

		KUNIT_EXPECT_EQ_MSG(test, policy.idle_select_mode,
				    casep->mode, "%s", casep->name);
		KUNIT_EXPECT_EQ_MSG(test, policy.heterogeneous,
				    casep->heterogeneous, "%s", casep->name);
		KUNIT_EXPECT_EQ_MSG(test, policy.mixed_cpu_types,
				    casep->mixed_cpu_types, "%s", casep->name);
		KUNIT_EXPECT_EQ_MSG(test, policy.unsupported_ecore_smt,
				    casep->unsupported_ecore_smt,
				    "%s", casep->name);
	}
}

static void sched_topology_group_class_test(struct kunit *test)
{
	struct sched_topology_test_masks *masks;
	bool single_class;

	if (nr_cpu_ids < 2) {
		kunit_skip(test, "at least two CPU bits are required");
		return;
	}

	masks = kunit_kzalloc(test, sizeof(*masks), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, masks);

	sched_topology_test_mask(&masks->active, BIT(0) | BIT(1));
	sched_topology_test_mask(&masks->pcore, BIT(0) | BIT(1));
	sched_topology_test_mask(&masks->ecore, 0);
	sched_topology_test_mask(&masks->unknown, 0);
	single_class = sched_group_has_single_class(&masks->active,
						    &masks->pcore,
						    &masks->ecore,
						    &masks->unknown);
	KUNIT_EXPECT_TRUE(test, single_class);

	sched_topology_test_mask(&masks->pcore, BIT(0));
	sched_topology_test_mask(&masks->ecore, BIT(1));
	single_class = sched_group_has_single_class(&masks->active,
						    &masks->pcore,
						    &masks->ecore,
						    &masks->unknown);
	KUNIT_EXPECT_FALSE(test, single_class);

	sched_topology_test_mask(&masks->pcore, 0);
	sched_topology_test_mask(&masks->ecore, 0);
	sched_topology_test_mask(&masks->unknown, BIT(0) | BIT(1));
	single_class = sched_group_has_single_class(&masks->active,
						    &masks->pcore,
						    &masks->ecore,
						    &masks->unknown);
	KUNIT_EXPECT_TRUE(test, single_class);
}

static struct kunit_case sched_topology_test_cases[] = {
	KUNIT_CASE(sched_topology_policy_test),
	KUNIT_CASE(sched_topology_group_class_test),
	{}
};

static struct kunit_suite sched_topology_test_suite = {
	.name = "sched-alt-topology",
	.test_cases = sched_topology_test_cases,
};

kunit_test_suite(sched_topology_test_suite);
