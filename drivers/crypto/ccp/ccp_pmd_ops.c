/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright(c) 2018 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <string.h>

#include <rte_common.h>
#include <rte_cryptodev_pmd.h>
#include <rte_malloc.h>

#include "ccp_pmd_private.h"
#include "ccp_dev.h"
#include "ccp_crypto.h"

static const struct rte_cryptodev_capabilities ccp_pmd_capabilities[] = {
	RTE_CRYPTODEV_END_OF_CAPABILITIES_LIST()
};

static int
ccp_pmd_config(struct rte_cryptodev *dev __rte_unused,
	       struct rte_cryptodev_config *config __rte_unused)
{
	return 0;
}

static int
ccp_pmd_start(struct rte_cryptodev *dev)
{
	return ccp_dev_start(dev);
}

static void
ccp_pmd_stop(struct rte_cryptodev *dev __rte_unused)
{

}

static int
ccp_pmd_close(struct rte_cryptodev *dev __rte_unused)
{
	return 0;
}

static void
ccp_pmd_info_get(struct rte_cryptodev *dev,
		 struct rte_cryptodev_info *dev_info)
{
	struct ccp_private *internals = dev->data->dev_private;

	if (dev_info != NULL) {
		dev_info->driver_id = dev->driver_id;
		dev_info->feature_flags = dev->feature_flags;
		dev_info->capabilities = ccp_pmd_capabilities;
		dev_info->max_nb_queue_pairs = internals->max_nb_qpairs;
		dev_info->sym.max_nb_sessions = internals->max_nb_sessions;
	}
}

static int
ccp_pmd_qp_release(struct rte_cryptodev *dev, uint16_t qp_id)
{
	struct ccp_qp *qp;

	if (dev->data->queue_pairs[qp_id] != NULL) {
		qp = (struct ccp_qp *)dev->data->queue_pairs[qp_id];
		rte_ring_free(qp->processed_pkts);
		rte_mempool_free(qp->batch_mp);
		rte_free(qp);
		dev->data->queue_pairs[qp_id] = NULL;
	}
	return 0;
}

static int
ccp_pmd_qp_set_unique_name(struct rte_cryptodev *dev,
		struct ccp_qp *qp)
{
	unsigned int n = snprintf(qp->name, sizeof(qp->name),
			"ccp_pmd_%u_qp_%u",
			dev->data->dev_id, qp->id);

	if (n > sizeof(qp->name))
		return -1;

	return 0;
}

static struct rte_ring *
ccp_pmd_qp_create_batch_info_ring(struct ccp_qp *qp,
				  unsigned int ring_size, int socket_id)
{
	struct rte_ring *r;

	r = rte_ring_lookup(qp->name);
	if (r) {
		if (r->size >= ring_size) {
			CCP_LOG_INFO(
				"Reusing ring %s for processed packets",
				qp->name);
			return r;
		}
		CCP_LOG_INFO(
			"Unable to reuse ring %s for processed packets",
			 qp->name);
		return NULL;
	}

	return rte_ring_create(qp->name, ring_size, socket_id,
			RING_F_SP_ENQ | RING_F_SC_DEQ);
}

static int
ccp_pmd_qp_setup(struct rte_cryptodev *dev, uint16_t qp_id,
		 const struct rte_cryptodev_qp_conf *qp_conf,
		 int socket_id, struct rte_mempool *session_pool)
{
	struct ccp_private *internals = dev->data->dev_private;
	struct ccp_qp *qp;
	int retval = 0;

	if (qp_id >= internals->max_nb_qpairs) {
		CCP_LOG_ERR("Invalid qp_id %u, should be less than %u",
			    qp_id, internals->max_nb_qpairs);
		return (-EINVAL);
	}

	/* Free memory prior to re-allocation if needed. */
	if (dev->data->queue_pairs[qp_id] != NULL)
		ccp_pmd_qp_release(dev, qp_id);

	/* Allocate the queue pair data structure. */
	qp = rte_zmalloc_socket("CCP Crypto PMD Queue Pair", sizeof(*qp),
					RTE_CACHE_LINE_SIZE, socket_id);
	if (qp == NULL) {
		CCP_LOG_ERR("Failed to allocate queue pair memory");
		return (-ENOMEM);
	}

	qp->dev = dev;
	qp->id = qp_id;
	dev->data->queue_pairs[qp_id] = qp;

	retval = ccp_pmd_qp_set_unique_name(dev, qp);
	if (retval) {
		CCP_LOG_ERR("Failed to create unique name for ccp qp");
		goto qp_setup_cleanup;
	}

	qp->processed_pkts = ccp_pmd_qp_create_batch_info_ring(qp,
			qp_conf->nb_descriptors, socket_id);
	if (qp->processed_pkts == NULL) {
		CCP_LOG_ERR("Failed to create batch info ring");
		goto qp_setup_cleanup;
	}

	qp->sess_mp = session_pool;

	/* mempool for batch info */
	qp->batch_mp = rte_mempool_create(
				qp->name,
				qp_conf->nb_descriptors,
				sizeof(struct ccp_batch_info),
				RTE_CACHE_LINE_SIZE,
				0, NULL, NULL, NULL, NULL,
				SOCKET_ID_ANY, 0);
	if (qp->batch_mp == NULL)
		goto qp_setup_cleanup;
	memset(&qp->qp_stats, 0, sizeof(qp->qp_stats));
	return 0;

qp_setup_cleanup:
	dev->data->queue_pairs[qp_id] = NULL;
	if (qp)
		rte_free(qp);
	return -1;
}

static int
ccp_pmd_qp_start(struct rte_cryptodev *dev __rte_unused,
		 uint16_t queue_pair_id __rte_unused)
{
	return -ENOTSUP;
}

static int
ccp_pmd_qp_stop(struct rte_cryptodev *dev __rte_unused,
		uint16_t queue_pair_id __rte_unused)
{
	return -ENOTSUP;
}

static uint32_t
ccp_pmd_qp_count(struct rte_cryptodev *dev)
{
	return dev->data->nb_queue_pairs;
}

static unsigned
ccp_pmd_session_get_size(struct rte_cryptodev *dev __rte_unused)
{
	return sizeof(struct ccp_session);
}

static int
ccp_pmd_session_configure(struct rte_cryptodev *dev,
			  struct rte_crypto_sym_xform *xform,
			  struct rte_cryptodev_sym_session *sess,
			  struct rte_mempool *mempool)
{
	int ret;
	void *sess_private_data;

	if (unlikely(sess == NULL || xform == NULL)) {
		CCP_LOG_ERR("Invalid session struct or xform");
		return -ENOMEM;
	}

	if (rte_mempool_get(mempool, &sess_private_data)) {
		CCP_LOG_ERR("Couldn't get object from session mempool");
		return -ENOMEM;
	}
	ret = ccp_set_session_parameters(sess_private_data, xform);
	if (ret != 0) {
		CCP_LOG_ERR("failed configure session parameters");

		/* Return session to mempool */
		rte_mempool_put(mempool, sess_private_data);
		return ret;
	}
	set_session_private_data(sess, dev->driver_id,
				 sess_private_data);

	return 0;
}

static void
ccp_pmd_session_clear(struct rte_cryptodev *dev,
		      struct rte_cryptodev_sym_session *sess)
{
	uint8_t index = dev->driver_id;
	void *sess_priv = get_session_private_data(sess, index);

	if (sess_priv) {
		struct rte_mempool *sess_mp = rte_mempool_from_obj(sess_priv);

		rte_mempool_put(sess_mp, sess_priv);
		memset(sess_priv, 0, sizeof(struct ccp_session));
		set_session_private_data(sess, index, NULL);
	}
}

struct rte_cryptodev_ops ccp_ops = {
		.dev_configure		= ccp_pmd_config,
		.dev_start		= ccp_pmd_start,
		.dev_stop		= ccp_pmd_stop,
		.dev_close		= ccp_pmd_close,

		.stats_get		= NULL,
		.stats_reset		= NULL,

		.dev_infos_get		= ccp_pmd_info_get,

		.queue_pair_setup	= ccp_pmd_qp_setup,
		.queue_pair_release	= ccp_pmd_qp_release,
		.queue_pair_start	= ccp_pmd_qp_start,
		.queue_pair_stop	= ccp_pmd_qp_stop,
		.queue_pair_count	= ccp_pmd_qp_count,

		.session_get_size	= ccp_pmd_session_get_size,
		.session_configure	= ccp_pmd_session_configure,
		.session_clear		= ccp_pmd_session_clear,
};

struct rte_cryptodev_ops *ccp_pmd_ops = &ccp_ops;
