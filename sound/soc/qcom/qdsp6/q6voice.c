// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/soc/qcom/apr.h>
#include "q6afe.h"
#include "q6core.h"
#include "q6cvp.h"
#include "q6cvs.h"
#include "q6mvm.h"
#include "q6voice-common.h"

#define VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS		0x00010F71
#define VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT		0x00010F77

struct q6voice_path_runtime {
	struct q6voice_session *sessions[Q6VOICE_SERVICE_COUNT];
	unsigned int started;
};

struct q6voice_path {
	struct q6voice *v;

	enum q6voice_path_type type;
	int tx_port, rx_port;
	u32 tx_topo, rx_topo;
	/* Serialize access to voice path session */
	struct mutex lock;
	struct q6voice_path_runtime *runtime;
};

struct q6voice {
	struct device *dev;
	bool cvd_v2_3;
	bool cvd_detected;
	struct q6voice_path paths[Q6VOICE_PATH_COUNT];
};

/*
 * CVD 2.3 and newer (SDM670, SM6150/SM7150, ...) want a different vocproc
 * set-up sequence. Downstream decides this from the CVP service's API version
 * as reported by AVCS: version 2 or newer gets the v3 create command plus the
 * endpoint format and a topology commit. "qcom,cvd-v2.3" in the DT still
 * forces it, for firmware that does not answer the version query.
 */
#define Q6VOICE_CVP_VERSION_2	2

static void q6voice_detect_cvd(struct q6voice *v)
{
	struct q6core_svc_api_info info = { };
	int ret;

	if (v->cvd_detected)
		return;
	v->cvd_detected = true;

	ret = q6core_get_svc_api_info(APR_SVC_ADSP_CVP, &info);
	if (ret) {
		dev_warn(v->dev, "could not read the CVP API version (%d), using the pre-2.3 sequence\n",
			 ret);
		return;
	}

	v->cvd_v2_3 = info.api_version >= Q6VOICE_CVP_VERSION_2;
	dev_info(v->dev, "CVP API version %u.%u: %s vocproc set-up\n",
		 info.api_version, info.api_branch_version,
		 v->cvd_v2_3 ? "CVD 2.3" : "legacy");
}

static inline struct q6voice_session *session_create(enum q6voice_path_type type,
						     int tx_port, int rx_port,
						     u32 tx_topo, u32 rx_topo)
{
	return q6cvp_session_create(type, tx_port, rx_port, tx_topo, rx_topo);
}

static struct q6voice_session *session_create_v3(struct device *dev,
						 enum q6voice_path_type type,
						 int tx_port, int rx_port,
						 u32 tx_topo, u32 rx_topo)
{
	struct q6voice_session *cvp;
	int ret;

	cvp = q6cvp_session_create_v3(type, tx_port, rx_port, tx_topo, rx_topo);
	if (IS_ERR(cvp))
		return cvp;

	/*
	 * CVD 2.3 does not take the endpoint format from the AFE port: it
	 * has to be told, and the topology committed, before the vocproc
	 * can be enabled (downstream: voice_send_cvp_create_cmd() followed by
	 * voice_send_cvp_channel_info_cmd/media_format_cmd/topology_commit).
	 */
	ret = q6cvp_send_channel_info(cvp, false);
	if (ret) {
		dev_err(dev, "failed to set cvp rx channel info: %d\n", ret);
		goto err;
	}

	ret = q6cvp_send_channel_info(cvp, true);
	if (ret) {
		dev_err(dev, "failed to set cvp tx channel info: %d\n", ret);
		goto err;
	}

	ret = q6cvp_send_media_format(cvp, rx_port, false);
	if (ret) {
		dev_err(dev, "failed to set cvp rx media format: %d\n", ret);
		goto err;
	}

	ret = q6cvp_send_media_format(cvp, tx_port, true);
	if (ret) {
		dev_err(dev, "failed to set cvp tx media format: %d\n", ret);
		goto err;
	}

	ret = q6cvp_topology_commit(cvp);
	if (ret) {
		dev_err(dev, "failed to commit cvp topology: %d\n", ret);
		goto err;
	}

	return cvp;

err:
	q6voice_session_release(cvp);
	return ERR_PTR(ret);
}

/*
 * Downstream unmutes TX on the vocproc (device) and on the stream at every
 * call start, so do the same rather than rely on the DSP's default. A failure
 * is logged, not fatal: the downlink works either way.
 */
static void q6voice_unmute_tx(struct q6voice_path *p, struct q6voice_session *cvp)
{
	struct device *dev = p->v->dev;
	struct q6voice_session *cvs = p->runtime->sessions[Q6VOICE_SERVICE_CVS];
	int ret;

	ret = q6voice_common_set_mute(cvp, true, false);
	dev_info(dev, "vocproc tx unmute: %d\n", ret);

	if (!cvs) {
		cvs = q6cvs_session_create(p->type);
		if (IS_ERR(cvs)) {
			dev_warn(dev, "no cvs session for the uplink unmute: %ld\n", PTR_ERR(cvs));
			return;
		}
		p->runtime->sessions[Q6VOICE_SERVICE_CVS] = cvs;
	}

	ret = q6voice_common_set_mute(cvs, true, false);
	dev_info(dev, "stream tx unmute: %d\n", ret);
}

static int q6voice_path_start(struct q6voice_path *p)
{
	struct device *dev = p->v->dev;
	struct q6voice_session *mvm, *cvp;
	int ret;

	dev_dbg(dev, "start path %d\n", p->type);

	mvm = p->runtime->sessions[Q6VOICE_SERVICE_MVM];
	if (!mvm) {
		mvm = q6mvm_session_create(p->type);
		if (IS_ERR(mvm)) {
			dev_err(dev, "failed to create mvm session: %ld\n",
				PTR_ERR(mvm));
			return PTR_ERR(mvm);
		}
		p->runtime->sessions[Q6VOICE_SERVICE_MVM] = mvm;
	}

	cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	if (!cvp) {
		q6voice_detect_cvd(p->v);

		if (p->v->cvd_v2_3)
			cvp = session_create_v3(dev, p->type,
						q6afe_get_port_id(p->tx_port),
						q6afe_get_port_id(p->rx_port),
						p->tx_topo, p->rx_topo);
		else
			cvp = session_create(p->type,
					     q6afe_get_port_id(p->tx_port),
					     q6afe_get_port_id(p->rx_port),
					     p->tx_topo, p->rx_topo);

		if (IS_ERR(cvp)) {
			dev_err(dev, "failed to create cvp session (tx port %#x, rx port %#x): %ld\n",
				q6afe_get_port_id(p->tx_port),
				q6afe_get_port_id(p->rx_port), PTR_ERR(cvp));
			return PTR_ERR(cvp);
		}
		p->runtime->sessions[Q6VOICE_SERVICE_CVP] = cvp;
	}

	ret = q6cvp_enable(cvp, true);
	if (ret) {
		dev_err(dev, "failed to enable cvp: %d\n", ret);
		goto cvp_err;
	}

	ret = q6mvm_attach(mvm, cvp, true);
	if (ret) {
		dev_err(dev, "failed to attach cvp to mvm: %d\n", ret);
		goto attach_err;
	}

	ret = q6mvm_start(mvm, true);
	if (ret) {
		dev_err(dev, "failed to start voice: %d\n", ret);
		goto start_err;
	}

	q6voice_unmute_tx(p, cvp);
	return ret;

start_err:
	q6mvm_start(mvm, false);
attach_err:
	q6mvm_attach(mvm, cvp, false);
cvp_err:
	q6cvp_enable(cvp, false);
	return ret;
}

int q6voice_start(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p = &v->paths[path];
	int ret = 0;

	mutex_lock(&p->lock);
	if (!p->runtime) {
		p->runtime = kzalloc_obj(*p->runtime, GFP_KERNEL);
		if (!p->runtime) {
			ret = -ENOMEM;
			goto out;
		}
	}

	if (p->runtime->started & BIT(capture)) {
		ret = -EALREADY;
		goto out;
	}

	p->runtime->started |= BIT(capture);

	/* FIXME: For now we only start if both RX/TX are active */
	if (p->runtime->started != 3)
		goto out;

	ret = q6voice_path_start(p);
	if (ret) {
		p->runtime->started &= ~BIT(capture);
		dev_err(v->dev, "failed to start path %d: %d\n", path, ret);
		goto out;
	}

out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_start);

static void q6voice_path_stop(struct q6voice_path *p)
{
	struct device *dev = p->v->dev;
	struct q6voice_session *mvm = p->runtime->sessions[Q6VOICE_SERVICE_MVM];
	struct q6voice_session *cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	int ret;

	dev_dbg(dev, "stop path %d\n", p->type);

	ret = q6mvm_start(mvm, false);
	if (ret)
		dev_err(dev, "failed to stop voice: %d\n", ret);

	ret = q6mvm_attach(mvm, cvp, false);
	if (ret)
		dev_err(dev, "failed to detach cvp from mvm: %d\n", ret);

	ret = q6cvp_enable(cvp, false);
	if (ret)
		dev_err(dev, "failed to disable cvp: %d\n", ret);
}

static void q6voice_path_destroy(struct q6voice_path *p)
{
	struct q6voice_path_runtime *runtime = p->runtime;
	enum q6voice_service_type svc;

	for (svc = 0; svc < Q6VOICE_SERVICE_COUNT; ++svc) {
		if (runtime->sessions[svc])
			q6voice_session_release(runtime->sessions[svc]);
	}

	p->runtime = NULL;
	kfree(runtime);
}

int q6voice_stop(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p = &v->paths[path];
	int ret = 0;

	mutex_lock(&p->lock);
	if (!p->runtime || !(p->runtime->started & BIT(capture)))
		goto out;

	if (p->runtime->started == 3)
		q6voice_path_stop(p);

	p->runtime->started &= ~BIT(capture);

	if (p->runtime->started == 0)
		q6voice_path_destroy(p);

out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_stop);

static void q6voice_free(void *data)
{
	struct q6voice *v = data;
	enum q6voice_path_type path;

	for (path = 0; path < Q6VOICE_PATH_COUNT; ++path) {
		struct q6voice_path *p = &v->paths[path];

		mutex_lock(&p->lock);
		if (p->runtime) {
			dev_warn(v->dev,
				 "q6voice_remove() called while path %d is active\n",
				 path);

			if (p->runtime->started == 3)
				q6voice_path_stop(p);
			q6voice_path_destroy(p);
		}
		mutex_unlock(&p->lock);
		mutex_destroy(&p->lock);
	}
}

struct q6voice *q6voice_create(struct device *dev, bool cvd_v2_3)
{
	struct q6voice *v;
	enum q6voice_path_type path;
	int ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return ERR_PTR(-ENOMEM);

	v->dev = dev;
	v->cvd_v2_3 = cvd_v2_3;
	/* a forced CVD 2.3 sequence needs no detection */
	v->cvd_detected = cvd_v2_3;

	for (path = 0; path < Q6VOICE_PATH_COUNT; ++path) {
		struct q6voice_path *p = &v->paths[path];

		p->v = v;
		p->type = path;
		p->tx_topo = VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS;
		p->rx_topo = VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT;
		mutex_init(&p->lock);
	}

	ret = devm_add_action(dev, q6voice_free, v);
	if (ret)
		return ERR_PTR(ret);

	return v;
}
EXPORT_SYMBOL_GPL(q6voice_create);

int q6voice_get_port(struct q6voice *v, enum q6voice_path_type path,
		     bool capture)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		return p->tx_port;
	else
		return p->rx_port;
}
EXPORT_SYMBOL_GPL(q6voice_get_port);

void q6voice_set_port(struct q6voice *v, enum q6voice_path_type path,
		      bool capture, int index)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		p->tx_port = index;
	else
		p->rx_port = index;
}
EXPORT_SYMBOL_GPL(q6voice_set_port);

u32 q6voice_get_topology(struct q6voice *v, enum q6voice_path_type path,
			 bool capture)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		return p->tx_topo;
	else
		return p->rx_topo;
}
EXPORT_SYMBOL_GPL(q6voice_get_topology);

void q6voice_set_topology(struct q6voice *v, enum q6voice_path_type path,
			  bool capture, u32 topo)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		p->tx_topo = topo;
	else
		p->rx_topo = topo;
}
EXPORT_SYMBOL_GPL(q6voice_set_topology);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6Voice driver");
MODULE_LICENSE("GPL");
