/*-
 * Copyright (c) 2008-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Martin Blix Grydeland <martin@varnish-software.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef VTEST_WITH_VTC_LOGEXPECT

/* SECTION: logexpect logexpect
 *
 * Reads the VSL and looks for records matching a given specification. It will
 * process records trying to match the first pattern, and when done, will
 * continue processing, trying to match the following pattern. If a pattern
 * isn't matched, the test will fail.
 *
 * logexpect threads are declared this way::
 *
 *         logexpect lNAME -v <id> [-g <grouping>] [-d 0|1] [-q query] \
 *                 [vsl arguments] {
 *                         expect <skip> <vxid> <tag> <regex>
 *                         expect <skip> <vxid> <tag> <regex>
 *                         ...
 *                 } [-start|-wait]
 *
 * And once declared, you can start them, or wait on them::
 *
 *         logexpect lNAME <-start|-wait>
 *
 * With:
 *
 * lNAME
 *         Name the logexpect thread, it must start with 'l'.
 *
 * \-v id
 *         Specify the varnish instance to use (most of the time, id=v1).
 *
 * \-g <session|request|vxid|raw
 *         Decide how records are grouped, see -g in ``man varnishlog`` for more
 *         information.
 *
 * \-d <0|1>
 *         Start processing log records at the head of the log instead of the
 *         tail.
 *
 * \-q query
 *         Filter records using a query expression, see ``man vsl-query`` for
 *         more information. Multiple -q options are not supported.
 *
 * \-m
 *         Also emit log records for misses (only for debugging)
 *
 * \-start
 *         Start the logexpect thread in the background.
 *
 * \-wait
 *         Wait for the logexpect thread to finish
 *
 * VSL arguments (similar to the varnishlog options):
 *
 * \-C
 *         Use caseless regex
 *
 * \-i <taglist>
 *         Include tags
 *
 * \-I <[taglist:]regex>
 *         Include by regex
 *
 * \-T <seconds>
 *         Transaction end timeout
 *
 * And the arguments of the specifications lines are:
 *
 * skip: [uint|*]
 *         Max number of record to skip
 *
 * vxid: [uint|*|=]
 *         vxid to match
 *
 * tag:  [tagname|*|=]
 *         Tag to match against
 *
 * regex:
 *         regular expression to match against (optional)
 *
 * For skip, vxid and tag, '*' matches anything, '=' expects the value of the
 * previous matched record.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "vapi/vsm.h"
#include "vapi/vsl.h"

#include "vtc.h"

#include "vtim.h"
#include "vre.h"

#define LE_ANY  (-1)
#define LE_LAST (-2)

struct logexp_test {
	unsigned			magic;
#define LOGEXP_TEST_MAGIC		0x6F62B350
	VTAILQ_ENTRY(logexp_test)	list;

	struct vsb			*str;
	int				vxid;
	int				tag;
	vre_t				*vre;
	int				skip_max;
};

struct logexp {
	unsigned			magic;
#define LOGEXP_MAGIC			0xE81D9F1B
	VTAILQ_ENTRY(logexp)		list;

	char				*name;
	char				*vname;
	struct vtclog			*vl;
	char				run;
	VTAILQ_HEAD(,logexp_test)	tests;

	struct logexp_test		*test;
	int				skip_cnt;
	int				vxid_last;
	int				tag_last;

	int				m_arg;
	int				d_arg;
	enum VSL_grouping_e		g_arg;
	char				*query;

	struct vsm			*vsm;
	struct VSL_data			*vsl;
	struct VSLQ			*vslq;
	pthread_t			tp;
};

static VTAILQ_HEAD(, logexp)		logexps =
	VTAILQ_HEAD_INITIALIZER(logexps);

static cmd_f cmd_logexp_expect;

static const struct cmds logexp_cmds[] = {
	{ "expect",		cmd_logexp_expect },
	{ NULL,			NULL },
};

static void
logexp_delete_tests(struct logexp *le)
{
	struct logexp_test *test;

	CHECK_OBJ_NOTNULL(le, LOGEXP_MAGIC);
	while (!VTAILQ_EMPTY(&le->tests)) {
		test = VTAILQ_FIRST(&le->tests);
		CHECK_OBJ_NOTNULL(test, LOGEXP_TEST_MAGIC);
		VTAILQ_REMOVE(&le->tests, test, list);
		VSB_destroy(&test->str);
		if (test->vre)
			VRE_free(&test->vre);
		FREE_OBJ(test);
	}
}

static void
logexp_delete(struct logexp *le)
{
	CHECK_OBJ_NOTNULL(le, LOGEXP_MAGIC);
	AZ(le->run);
	AN(le->vsl);
	VSL_Delete(le->vsl);
	AZ(le->vslq);
	logexp_delete_tests(le);
	free(le->name);
	free(le->vname);
	free(le->query);
	VSM_Destroy(&le->vsm);
	vtc_logclose(le->vl);
	FREE_OBJ(le);
}

static struct logexp *
logexp_new(const char *name, const char *varg)
{
	struct logexp *le;
	struct vsb *n_arg;

	ALLOC_OBJ(le, LOGEXP_MAGIC);
	AN(le);
	REPLACE(le->name, name);
	le->vl = vtc_logopen("%s", name);
	vtc_log_set_cmd(le->vl, logexp_cmds);
	VTAILQ_INIT(&le->tests);

	le->d_arg = 0;
	le->g_arg = VSL_g_vxid;
	le->vsm = VSM_New();
	le->vsl = VSL_New();
	AN(le->vsm);
	AN(le->vsl);

	VTAILQ_INSERT_TAIL(&logexps, le, list);

	REPLACE(le->vname, varg);

	n_arg = macro_expandf(le->vl, "${tmpdir}/%s", varg);
	if (n_arg == NULL)
		vtc_fatal(le->vl, "-v argument problems");
	if (VSM_Arg(le->vsm, 'n', VSB_data(n_arg)) <= 0)
		vtc_fatal(le->vl, "-v argument error: %s",
		    VSM_Error(le->vsm));
	VSB_destroy(&n_arg);
	if (VSM_Attach(le->vsm, -1))
		vtc_fatal(le->vl, "VSM_Attach: %s", VSM_Error(le->vsm));
	return (le);
}

static void
logexp_next(struct logexp *le)
{
	CHECK_OBJ_NOTNULL(le, LOGEXP_MAGIC);

	if (le->test) {
		CHECK_OBJ_NOTNULL(le->test, LOGEXP_TEST_MAGIC);
		le->test = VTAILQ_NEXT(le->test, list);
	} else
		le->test = VTAILQ_FIRST(&le->tests);

	CHECK_OBJ_ORNULL(le->test, LOGEXP_TEST_MAGIC);
	if (le->test)
		vtc_log(le->vl, 3, "expecting| %s", VSB_data(le->test->str));
}

static int v_matchproto_(VSLQ_dispatch_f)
logexp_dispatch(struct VSL_data *vsl, struct VSL_transaction * const pt[],
    void *priv)
{
	struct logexp *le;
	struct VSL_transaction *t;
	int i;
	int ok, skip;
	int vxid, tag, type, len;
	const char *legend, *data;

	CAST_OBJ_NOTNULL(le, priv, LOGEXP_MAGIC);

	for (i = 0; (t = pt[i]) != NULL; i++) {
		while (1 == VSL_Next(t->c)) {
			if (!VSL_Match(vsl, t->c))
				continue;

			CHECK_OBJ_NOTNULL(le->test, LOGEXP_TEST_MAGIC);
			AN(t->c->rec.ptr);
			vxid = VSL_ID(t->c->rec.ptr);
			tag = VSL_TAG(t->c->rec.ptr);
			data = VSL_CDATA(t->c->rec.ptr);
			len = VSL_LEN(t->c->rec.ptr) - 1;

			if (tag == SLT__Batch || tag == SLT_Witness)
				continue;

			ok = 1;
			if (le->test->vxid == LE_LAST) {
				if (le->vxid_last != vxid)
					ok = 0;
			} else if (le->test->vxid >= 0) {
				if (le->test->vxid != vxid)
					ok = 0;
			}
			if (le->test->tag == LE_LAST) {
				if (le->tag_last != tag)
					ok = 0;
			} else if (le->test->tag >= 0) {
				if (le->test->tag != tag)
					ok = 0;
			}
			if (le->test->vre &&
			    le->test->tag >= 0 &&
			    le->test->tag == tag &&
			    VRE_ERROR_NOMATCH == VRE_exec(le->test->vre, data,
				len, 0, 0, NULL, 0, NULL))
				ok = 0;

			skip = 0;
			if (!ok && (le->test->skip_max == LE_ANY ||
				le->test->skip_max > le->skip_cnt))
				skip = 1;

			if (ok)
				legend = "match";
			else if (skip && le->m_arg)
				legend = "miss";
			else if (skip)
				legend = NULL;
			else
				legend = "err";
			type = VSL_CLIENT(t->c->rec.ptr) ? 'c' :
			    VSL_BACKEND(t->c->rec.ptr) ? 'b' : '-';

			if (legend != NULL)
				vtc_log(le->vl, 4, "%-5s| %10u %-15s %c %.*s",
				    legend, vxid, VSL_tags[tag], type, len,
				    data);

			if (ok) {
				le->vxid_last = vxid;
				le->tag_last = tag;
				le->skip_cnt = 0;
				logexp_next(le);
				if (le->test == NULL)
					/* End of test script */
					return (1);
			} else if (skip)
				le->skip_cnt++;
			else {
				/* Signal fail */
				return (2);
			}
		}
	}

	return (0);
}

static void *
logexp_thread(void *priv)
{
	struct logexp *le;
	int i;

	CAST_OBJ_NOTNULL(le, priv, LOGEXP_MAGIC);
	AN(le->run);
	AN(le->vsm);
	AN(le->vslq);

	AZ(le->test);
	vtc_log(le->vl, 4, "begin|");
	if (le->query != NULL)
		vtc_log(le->vl, 4, "qry| %s", le->query);
	logexp_next(le);
	while (le->test) {
		i = VSLQ_Dispatch(le->vslq, logexp_dispatch, le);
		if (i == 2)
			vtc_fatal(le->vl, "bad| expectation failed");
		else if (i < 0)
			vtc_fatal(le->vl, "bad| dispatch failed (%d)", i);
		else if (i == 0 && le->test)
			VTIM_sleep(0.01);
	}
	vtc_log(le->vl, 4, "end|");

	return (NULL);
}

static void
logexp_close(struct logexp *le)
{

	CHECK_OBJ_NOTNULL(le, LOGEXP_MAGIC);
	AN(le->vsm);
	if (le->vslq)
		VSLQ_Delete(&le->vslq);
	AZ(le->vslq);
}

static void
logexp_start(struct logexp *le)
{
	struct VSL_cursor *c;

	CHECK_OBJ_NOTNULL(le, LOGEXP_MAGIC);
	AN(le->vsl);
	AZ(le->vslq);

	AN(le->vsl);
	(void)VSM_Status(le->vsm);
	c = VSL_CursorVSM(le->vsl, le->vsm,
	    (le->d_arg ? 0 : VSL_COPT_TAIL) | VSL_COPT_BATCH);
	if (c == NULL)
		vtc_fatal(le->vl, "VSL_CursorVSM: %s", VSL_Error(le->vsl));
	le->vslq = VSLQ_New(le->vsl, &c, le->g_arg, le->query);
	if (le->vslq == NULL) {
		VSL_DeleteCursor(c);
		vtc_fatal(le->vl, "VSLQ_New: %s", VSL_Error(le->vsl));
	}
	AZ(c);

	le->test = NULL;
	le->skip_cnt = 0;
	le->vxid_last = le->tag_last = -1;
	le->run = 1;
	AZ(pthread_create(&le->tp, NULL, logexp_thread, le));
}

static void
logexp_wait(struct logexp *le)
{
	void *res;

	CHECK_OBJ_NOTNULL(le, LOGEXP_MAGIC);
	vtc_log(le->vl, 2, "Waiting for logexp");
	AZ(pthread_join(le->tp, &res));
	logexp_close(le);
	if (res != NULL && !vtc_stop)
		vtc_fatal(le->vl, "logexp returned \"%p\"", (char *)res);
	le->run = 0;
}

static void
cmd_logexp_expect(CMD_ARGS)
{
	struct logexp *le;
	int skip_max;
	int vxid;
	int tag;
	vre_t *vre;
	const char *err;
	int pos;
	struct logexp_test *test;
	char *end;

	CAST_OBJ_NOTNULL(le, priv, LOGEXP_MAGIC);
	if (av[1] == NULL || av[2] == NULL || av[3] == NULL)
		vtc_fatal(vl, "Syntax error");

	if (av[4] != NULL && av[5] != NULL)
		vtc_fatal(vl, "Syntax error");

	if (!strcmp(av[1], "*"))
		skip_max = LE_ANY;
	else {
		skip_max = (int)strtol(av[1], &end, 10);
		if (*end != '\0' || skip_max < 0)
			vtc_fatal(vl, "Not a positive integer: '%s'", av[1]);
	}
	if (!strcmp(av[2], "*"))
		vxid = LE_ANY;
	else if (!strcmp(av[2], "="))
		vxid = LE_LAST;
	else {
		vxid = (int)strtol(av[2], &end, 10);
		if (*end != '\0' || vxid < 0)
			vtc_fatal(vl, "Not a positive integer: '%s'", av[2]);
	}
	if (!strcmp(av[3], "*"))
		tag = LE_ANY;
	else if (!strcmp(av[3], "="))
		tag = LE_LAST;
	else {
		tag = VSL_Name2Tag(av[3], strlen(av[3]));
		if (tag < 0)
			vtc_fatal(vl, "Unknown tag name: '%s'", av[3]);
	}
	vre = NULL;
	if (av[4]) {
		vre = VRE_compile(av[4], 0, &err, &pos);
		if (vre == NULL)
			vtc_fatal(vl, "Regex error (%s): '%s' pos %d",
			    err, av[4], pos);
	}

	ALLOC_OBJ(test, LOGEXP_TEST_MAGIC);
	AN(test);
	test->str = VSB_new_auto();
	AN(test->str);
	AZ(VSB_printf(test->str, "%s %s %s %s ", av[0], av[1], av[2], av[3]));
	if (av[4])
		VSB_quote(test->str, av[4], -1, 0);
	AZ(VSB_finish(test->str));
	test->skip_max = skip_max;
	test->vxid = vxid;
	test->tag = tag;
	test->vre = vre;
	VTAILQ_INSERT_TAIL(&le->tests, test, list);
}

static void
logexp_spec(struct logexp *le, const char *spec)
{
	CHECK_OBJ_NOTNULL(le, LOGEXP_MAGIC);

	logexp_delete_tests(le);

	parse_string(le->vl, le, spec);
}

void
cmd_logexpect(CMD_ARGS)
{
	struct logexp *le, *le2;
	int i;

	(void)priv;

	if (av == NULL) {
		/* Reset and free */
		VTAILQ_FOREACH_SAFE(le, &logexps, list, le2) {
			CHECK_OBJ_NOTNULL(le, LOGEXP_MAGIC);
			VTAILQ_REMOVE(&logexps, le, list);
			if (le->run) {
				(void)pthread_cancel(le->tp);
				logexp_wait(le);
			}
			logexp_delete(le);
		}
		return;
	}

	AZ(strcmp(av[0], "logexpect"));
	av++;

	VTC_CHECK_NAME(vl, av[0], "Logexpect", 'l');
	VTAILQ_FOREACH(le, &logexps, list) {
		if (!strcmp(le->name, av[0]))
			break;
	}
	if (le == NULL) {
		if (strcmp(av[1], "-v") || av[2] == NULL)
			vtc_fatal(vl, "new logexp lacks -v");
		le = logexp_new(av[0], av[2]);
		av += 2;
	}
	av++;

	for (; *av != NULL; av++) {
		if (vtc_error)
			break;
		if (!strcmp(*av, "-wait")) {
			if (!le->run)
				vtc_fatal(le->vl, "logexp not -started '%s'",
					*av);
			logexp_wait(le);
			continue;
		}

		/*
		 * We do an implict -wait if people muck about with a
		 * running logexp.
		 */
		if (le->run)
			logexp_wait(le);
		AZ(le->run);

		if (!strcmp(*av, "-v")) {
			if (av[1] == NULL || strcmp(av[1], le->vname))
				vtc_fatal(le->vl, "-v argument cannot change");
			av++;
			continue;
		}
		if (!strcmp(*av, "-d")) {
			if (av[1] == NULL)
				vtc_fatal(le->vl, "Missing -d argument");
			le->d_arg = atoi(av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-g")) {
			if (av[1] == NULL)
				vtc_fatal(le->vl, "Missing -g argument");
			i = VSLQ_Name2Grouping(av[1], strlen(av[1]));
			if (i < 0)
				vtc_fatal(le->vl, "Unknown grouping '%s'",
				    av[1]);
			le->g_arg = (enum VSL_grouping_e)i;
			av++;
			continue;
		}
		if (!strcmp(*av, "-q")) {
			if (av[1] == NULL)
				vtc_fatal(le->vl, "Missing -q argument");
			REPLACE(le->query, av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-m")) {
			le->m_arg = !le->m_arg;
			continue;
		}
		if (!strcmp(*av, "-start")) {
			logexp_start(le);
			continue;
		}
		if (!strcmp(*av, "-run")) {
			logexp_start(le);
			logexp_wait(le);
			continue;
		}
		if (**av == '-') {
			if (av[1] != NULL) {
				if (VSL_Arg(le->vsl, av[0][1], av[1])) {
					av++;
					continue;
				}
				vtc_fatal(le->vl, "%s", VSL_Error(le->vsl));
			}
			vtc_fatal(le->vl, "Unknown logexp argument: %s", *av);
		}
		logexp_spec(le, *av);
	}
}

#endif /* VTEST_WITH_VTC_LOGEXPECT */
