/*
 * Copyright 2015 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define MODULE_TAG "mpp_device"

#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "mpp_env.h"
#include "mpp_log.h"
#include "mpp_mem.h"
#include "mpp_time.h"
#include "mpp_common.h"

#include "mpp_device.h"
#include "mpp_device_msg.h"
#include "mpp_platform.h"

#include "vpu.h"

#define EXTRA_INFO_MAGIC                    (0x4C4A46)

#define VPU_IOC_MAGIC                       'l'

#define VPU_IOC_SET_CLIENT_TYPE             _IOW(VPU_IOC_MAGIC, 1, unsigned long)
#define VPU_IOC_SET_REG                     _IOW(VPU_IOC_MAGIC, 3, unsigned long)
#define VPU_IOC_GET_REG                     _IOW(VPU_IOC_MAGIC, 4, unsigned long)

#define VPU_IOC_SET_CLIENT_TYPE_U32         _IOW(VPU_IOC_MAGIC, 1, unsigned int)

#define VPU_IOC_WRITE(nr, size)             _IOC(_IOC_WRITE, VPU_IOC_MAGIC, (nr), (size))

/* Use 'v' as magic number */
#define MPP_IOC_MAGIC                       'v'
#define MPP_IOC_CFG_V1                      _IOW(MPP_IOC_MAGIC, 1, unsigned int)
#define MAX_REQ_NUM                         16

typedef struct MppReq_t {
    RK_U32 *req;
    RK_U32  size;
} MppReq;

typedef struct mppReqV1_t {
    RK_U32 cmd;
    RK_U32 flag;
    RK_U32 size;
    RK_U32 offset;
    RK_U64 data_ptr;
} MppReqV1;

#define MAX_TIME_RECORD                     4

typedef struct MppDevCtxImpl_t {
    MppCtxType type;
    MppCodingType coding;
    RK_S32 client_type;
    RK_U32 platform;        // platfrom for vcodec to init
    RK_U32 mmu_status;      // 0 disable, 1 enable
    RK_U32 pp_enable;       // postprocess, 0 disable, 1 enable
    RK_S32 vpu_fd;

    RK_S64 time_start[MAX_TIME_RECORD];
    RK_S64 time_end[MAX_TIME_RECORD];
    RK_S32 idx_send;
    RK_S32 idx_wait;
    RK_S32 idx_total;

    /*
     * 0 - vcodec_service SEG_REG / GET_REG ioctl set
     * 1 - mpp_service MPP_IOC_CFG_V1 ioctl set
     */
    RK_S32 ioctl_version;

    RK_S32 req_cnt;
    MppReqV1 reqs[MAX_REQ_NUM];

    /* support max cmd buttom  */
    RK_U32 support_cmd_butt;
    RK_U32 query_cmd_butt;
    RK_U32 init_cmd_butt;
    RK_U32 send_cmd_butt;
    RK_U32 poll_cmd_butt;
    RK_U32 control_cmd_butt;
} MppDevCtxImpl;

#define MPP_DEVICE_DBG_FUNC                 (0x00000001)
#define MPP_DEVICE_DBG_DETAIL               (0x00000002)
#define MPP_DEVICE_DBG_HW_SUPPORT           (0x00000008)

#define MPP_DEVICE_DBG_REG                  (0x00000010)
#define MPP_DEVICE_DBG_TIME                 (0x00000020)

#define mpp_dev_dbg(flag, fmt, ...)         _mpp_dbg(mpp_device_debug, flag, fmt, ## __VA_ARGS__)
#define mpp_dev_dbg_f(flag, fmt, ...)       _mpp_dbg_f(mpp_device_debug, flag, fmt, ## __VA_ARGS__)

#define mpp_dev_dbg_func(fmt, ...)          mpp_dev_dbg_f(MPP_DEVICE_DBG_FUNC, fmt, ## __VA_ARGS__)
#define mpp_dev_dbg_detail(fmt, ...)        mpp_dev_dbg_f(MPP_DEVICE_DBG_DETAIL, fmt, ## __VA_ARGS__)
#define mpp_dev_dbg_hw_cap(fmt, ...)        mpp_dev_dbg(MPP_DEVICE_DBG_HW_SUPPORT, fmt, ## __VA_ARGS__)

#define mpp_dev_dbg_reg(fmt, ...)           mpp_dev_dbg(MPP_DEVICE_DBG_REG, fmt, ## __VA_ARGS__)
#define mpp_dev_dbg_time(fmt, ...)          mpp_dev_dbg(MPP_DEVICE_DBG_TIME, fmt, ## __VA_ARGS__)

#if __SIZEOF_POINTER__ == 4
#define REQ_DATA_PTR(ptr) ((RK_U32)ptr)
#elif __SIZEOF_POINTER__ == 8
#define REQ_DATA_PTR(ptr) ((RK_U64)ptr)
#endif

static RK_U32 mpp_device_debug = 0;

static MPP_RET mpp_probe_cmd_butt(RK_S32 dev, MppDevCtxImpl *p)
{
    MPP_RET ret = MPP_OK;
    static RK_U32 probe_support_cmd = 0;

    if (probe_support_cmd) {
        p->support_cmd_butt = probe_support_cmd;
        return MPP_OK;
    }

    p->support_cmd_butt = probe_support_cmd = access("/proc/mpp_service/support_cmd", F_OK) ? 0 : 1;
    if (p->support_cmd_butt) {
        MppReqV1 mpp_req;

        memset(&mpp_req, 0, sizeof(mpp_req));
        p->query_cmd_butt = MPP_CMD_QUERY_BASE;
        mpp_req.cmd = MPP_CMD_QUERY_CMD_SUPPORT;
        mpp_req.data_ptr = REQ_DATA_PTR(&p->query_cmd_butt);

        ret = (RK_S32)ioctl(dev, MPP_IOC_CFG_V1, &mpp_req);
        if (ret)
            mpp_err_f("query cmd support error %s.\n", strerror(errno));

        p->init_cmd_butt = MPP_CMD_INIT_BASE;
        mpp_req.data_ptr = REQ_DATA_PTR(&p->init_cmd_butt);
        ret = (RK_S32)ioctl(dev, MPP_IOC_CFG_V1, &mpp_req);
        if (ret)
            mpp_err_f("query INIT_CMD_BUTT error %s.\n", strerror(errno));

        p->send_cmd_butt = MPP_CMD_SEND_BASE;
        mpp_req.data_ptr = REQ_DATA_PTR(&p->send_cmd_butt);
        ret = (RK_S32)ioctl(dev, MPP_IOC_CFG_V1, &mpp_req);
        if (ret)
            mpp_err_f("query SEND_CMD_BUTT error %s.\n", strerror(errno));

        p->poll_cmd_butt = MPP_CMD_POLL_BASE;
        mpp_req.data_ptr = REQ_DATA_PTR(&p->poll_cmd_butt);
        ret = (RK_S32)ioctl(dev, MPP_IOC_CFG_V1, &mpp_req);
        if (ret)
            mpp_err_f("query POLL_CMD_BUTT error %s.\n", strerror(errno));

        p->control_cmd_butt = MPP_CMD_CONTROL_BASE;
        mpp_req.data_ptr = REQ_DATA_PTR(&p->control_cmd_butt);
        ret = (RK_S32)ioctl(dev, MPP_IOC_CFG_V1, &mpp_req);
        if (ret)
            mpp_err_f("query CONTROL_CMD_BUTT error %s.\n", strerror(errno));
    }

    return ret;
}

static MPP_RET mpp_check_cmd_valid(RK_U32 cmd, MppDevCtxImpl *p)
{
    MPP_RET ret = MPP_OK;

    if (p->support_cmd_butt > 0) {
        RK_U32 found = 0;

        found = (cmd < p->query_cmd_butt) ? 1 : 0;
        found = (cmd >= MPP_CMD_INIT_BASE && cmd < p->init_cmd_butt) ? 1 : found;
        found = (cmd >= MPP_CMD_SEND_BASE && cmd < p->send_cmd_butt) ? 1 : found;
        found = (cmd >= MPP_CMD_POLL_BASE && cmd < p->poll_cmd_butt) ? 1 : found;
        found = (cmd >= MPP_CMD_CONTROL_BASE && cmd < p->control_cmd_butt) ? 1 : found;

        ret = found ? MPP_OK : MPP_NOK;
    }

    return ret;
}

static RK_U32 mpp_probe_hw_support(RK_S32 dev)
{
    RK_S32 ret;
    RK_U32 flag = 0;
    MppReqV1 mpp_req;

    mpp_req.cmd = MPP_CMD_PROBE_HW_SUPPORT;
    mpp_req.flag = 0;
    mpp_req.size = 0;
    mpp_req.offset = 0;
    mpp_req.data_ptr = REQ_DATA_PTR(&flag);

    ret = (RK_S32)ioctl(dev, MPP_IOC_CFG_V1, &mpp_req);
    if (ret) {
        mpp_err_f("probe hw support error %s.\n", strerror(errno));
        flag = 0;
    } else {
        mpp_refresh_vcodec_type(flag);
        mpp_dev_dbg_hw_cap("vcodec_support %08x\n", flag);
    }

    return flag;
}

static RK_U32 mpp_get_hw_id(RK_S32 dev)
{
    RK_S32 ret;
    RK_U32 flag = 0;
    MppReqV1 mpp_req;

    mpp_req.cmd = MPP_CMD_QUERY_HW_ID;
    mpp_req.flag = 0;
    mpp_req.size = 0;
    mpp_req.offset = 0;
    mpp_req.data_ptr = REQ_DATA_PTR(&flag);

    ret = (RK_S32)ioctl(dev, MPP_IOC_CFG_V1, &mpp_req);
    if (ret) {
        mpp_err_f("get hw id error %s.\n", strerror(errno));
        flag = 0;
    }

    mpp_dev_dbg_hw_cap("hardware version 0x%8x", flag);
    return flag;
}

static RK_S32 mpp_device_set_client_type(MppDevCtx ctx, int dev, RK_S32 client_type)
{
    RK_S32 ret;
    static RK_S32 mpp_device_ioctl_version = -1;
    MppDevCtxImpl *p = (MppDevCtxImpl *)ctx;

    if (p->ioctl_version > 0) {
        MppReqV1 mpp_req;
        RK_U32 client_data = client_type;

        mpp_req.cmd = MPP_CMD_INIT_CLIENT_TYPE;
        mpp_req.flag = 0;
        mpp_req.size = sizeof(client_data);
        mpp_req.offset = 0;
        mpp_req.data_ptr = REQ_DATA_PTR(&client_data);
        ret = (RK_S32)ioctl(dev, MPP_IOC_CFG_V1, &mpp_req);
    } else {
        if (mpp_device_ioctl_version < 0) {
            ret = ioctl(dev, VPU_IOC_SET_CLIENT_TYPE, (unsigned long)client_type);
            if (!ret) {
                mpp_device_ioctl_version = 0;
            } else {
                ret = ioctl(dev, VPU_IOC_SET_CLIENT_TYPE_U32, (RK_U32)client_type);
                if (!ret)
                    mpp_device_ioctl_version = 1;
            }
            mpp_assert(ret == 0);
        } else {
            RK_U32 cmd = (mpp_device_ioctl_version == 0) ?
                         (VPU_IOC_SET_CLIENT_TYPE) :
                         (VPU_IOC_SET_CLIENT_TYPE_U32);

            ret = ioctl(dev, cmd, client_type);
        }
    }
    if (ret)
        mpp_err_f("set client type failed ret %d errno %d\n", ret, errno);

    return ret;
}

static RK_S32 mpp_device_get_client_type(MppDevCtx ctx, MppCtxType type, MppCodingType coding)
{
    RK_S32 client_type = -1;
    MppDevCtxImpl *p;

    if (NULL == ctx || type >= MPP_CTX_BUTT ||
        (coding >= MPP_VIDEO_CodingMax || coding <= MPP_VIDEO_CodingUnused)) {
        mpp_err_f("found NULL input ctx %p coding %d type %d\n", ctx, coding, type);
        return MPP_ERR_NULL_PTR;
    }

    p = (MppDevCtxImpl *)ctx;

    if (p->ioctl_version > 0) {
        if (p->pp_enable)
            client_type = (p->platform & HAVE_VDPU1) ?
                          VPU_CLIENT_VDPU1_PP : VPU_CLIENT_VDPU2_PP;
        else {
            RK_U32 i = 0;

            while (i < 32 && !((p->platform >> i) & 0x1)) i++;

            client_type = i;
        }
    } else { /* ioctl mode is original */
        if (type == MPP_CTX_ENC)
            client_type = VPU_ENC;
        else /* MPP_CTX_DEC */
            client_type = p->pp_enable ? VPU_DEC_PP : VPU_DEC;
    }

    return client_type;
}

MPP_RET mpp_device_init(MppDevCtx *ctx, MppDevCfg *cfg)
{
    RK_S32 dev = -1;
    const char *name = NULL;
    MppDevCtxImpl *p;

    if (NULL == ctx || NULL == cfg) {
        mpp_err_f("found NULL input ctx %p cfg %p\n", ctx, cfg);
        return MPP_ERR_NULL_PTR;
    }

    mpp_dev_dbg_func("enter %p coding %d type %d platform %x\n",
                     ctx, cfg->coding, cfg->type, cfg->platform);

    *ctx = NULL;

    mpp_env_get_u32("mpp_device_debug", &mpp_device_debug, 0);

    p = mpp_calloc(MppDevCtxImpl, 1);
    if (NULL == p) {
        mpp_err_f("malloc failed\n");
        return MPP_ERR_MALLOC;
    }

    p->coding = cfg->coding;
    p->type = cfg->type;
    p->platform = cfg->platform;
    p->pp_enable = cfg->pp_enable;
    p->ioctl_version = mpp_get_ioctl_version();

    if (p->platform)
        name = mpp_get_platform_dev_name(p->type, p->coding, p->platform);
    else
        name = mpp_get_vcodec_dev_name(p->type, p->coding);
    if (name) {
        dev = open(name, O_RDWR);
        if (dev > 0) {
            RK_S32 client_type;
            RK_S32 ret;

            /* if ioctl_version is 1, query hw supprot*/
            if (p->ioctl_version > 0) {
                mpp_probe_cmd_butt(dev, p);
                mpp_probe_hw_support(dev);
            }

            client_type = mpp_device_get_client_type(p, p->type, p->coding);
            ret = mpp_device_set_client_type(p, dev, client_type);
            if (ret) {
                close(dev);
                dev = -2;
            }
            p->client_type = client_type;
        } else
            mpp_err_f("failed to open device %s, errno %d, error msg: %s\n",
                      name, errno, strerror(errno));
    } else
        mpp_err_f("failed to find device for coding %d type %d\n", p->coding, p->type);

    *ctx = p;
    p->vpu_fd = dev;
    if (p->ioctl_version > 0)
        cfg->hw_id = mpp_get_hw_id(dev);
    else
        cfg->hw_id = 0;

    mpp_dev_dbg_func("leave %p\n", dev);
    return MPP_OK;
}

MPP_RET mpp_device_deinit(MppDevCtx ctx)
{
    MppDevCtxImpl *p;

    if (NULL == ctx) {
        mpp_err_f("found NULL input ctx %p\n", ctx);
        return MPP_ERR_NULL_PTR;
    }

    mpp_dev_dbg_func("enter %p\n", ctx);

    p = (MppDevCtxImpl *)ctx;

    if (p->vpu_fd > 0) {
        close(p->vpu_fd);
    } else {
        mpp_err_f("invalid negtive file handle,\n");
    }
    mpp_free(p);

    mpp_dev_dbg_func("leave %p\n", ctx);
    return MPP_OK;
}

MPP_RET mpp_device_add_request(MppDevCtx ctx, MppDevReqV1 *req)
{
    MppDevCtxImpl *p = (MppDevCtxImpl *)ctx;

    mpp_dev_dbg_func("enter %p req %p\n", ctx, req);

    if (NULL == p) {
        mpp_err_f("found NULL input ctx %p\n", ctx);
        return MPP_ERR_NULL_PTR;
    }

    if (p->ioctl_version <= 0) {
        mpp_err_f("ctx %p can't add request without /dev/mpp_service\n", ctx);
        return MPP_ERR_PERM;
    }

    if (p->req_cnt >= MAX_REQ_NUM) {
        mpp_err_f("ctx %p request count %d overfow\n", ctx, p->req_cnt);
        return MPP_ERR_VALUE;
    }

    if (mpp_check_cmd_valid(req->cmd, p)) {
        mpp_err_f("ctx %p cmd 0x%08x not support\n", ctx, req->cmd);
        return MPP_ERR_VALUE;
    }

    if (!p->req_cnt)
        memset(p->reqs, 0, sizeof(p->reqs));

    MppReqV1 *mpp_req = &p->reqs[p->req_cnt];

    mpp_req->cmd = req->cmd;
    mpp_req->flag = req->flag;
    mpp_req->size =  req->size;
    mpp_req->offset = req->offset;
    mpp_req->data_ptr = REQ_DATA_PTR(req->data);
    p->req_cnt++;

    mpp_dev_dbg_detail("enter %p cnt %d cmd %08x flag %x size %3x offset %08x data %p\n",
                       ctx, p->req_cnt, req->cmd, req->flag,
                       req->size, req->offset, req->data);

    mpp_dev_dbg_func("leave %p req %p\n", ctx, req);
    return MPP_OK;
}

MPP_RET mpp_device_send_request(MppDevCtx ctx)
{
    MppDevCtxImpl *p = (MppDevCtxImpl *)ctx;;

    mpp_dev_dbg_func("enter %p\n", ctx);

    if (NULL == p) {
        mpp_err_f("found NULL input ctx %p\n", ctx);
        return MPP_ERR_NULL_PTR;
    }

    if (p->ioctl_version <= 0) {
        mpp_err_f("ctx %p can't add request without /dev/mpp_service\n", ctx);
        return MPP_ERR_PERM;
    }

    if (p->req_cnt <= 0 || p->req_cnt > MAX_REQ_NUM) {
        mpp_err_f("ctx %p invalid request count %d\n", ctx, p->req_cnt);
        return MPP_ERR_VALUE;
    }

    /* setup flag for multi message request */
    if (p->req_cnt > 1) {
        RK_S32 i;

        for (i = 0; i < p->req_cnt; i++)
            p->reqs[i].flag |= MPP_FLAGS_MULTI_MSG;
    }
    p->reqs[p->req_cnt - 1].flag |=  MPP_FLAGS_LAST_MSG;

    mpp_dev_dbg_detail("enter %p cnt %d\n", ctx, p->req_cnt);

    MPP_RET ret = (RK_S32)ioctl(p->vpu_fd, MPP_IOC_CFG_V1, &p->reqs[0]);
    if (ret) {
        mpp_err_f("ioctl MPP_IOC_CFG_V1 failed ret %d errno %d %s\n",
                  ret, errno, strerror(errno));
        ret = errno;
    }

    p->req_cnt = 0;

    mpp_dev_dbg_func("leave %p\n", ctx);
    return ret;
}

MPP_RET mpp_device_send_single_request(MppDevCtx ctx, MppDevReqV1 *req)
{
    MPP_RET ret = MPP_OK;

    MppDevCtxImpl *p = (MppDevCtxImpl *)ctx;

    mpp_dev_dbg_func("enter %p req %p\n", ctx, req);

    if (NULL == p) {
        mpp_err_f("found NULL input ctx %p\n", ctx);
        return MPP_ERR_NULL_PTR;
    }

    if (p->ioctl_version <= 0) {
        mpp_err_f("ctx %p can't add request without /dev/mpp_service\n", ctx);
        return MPP_ERR_PERM;
    }

    ret = (RK_S32)ioctl(p->vpu_fd, MPP_IOC_CFG_V1, req);
    if (ret) {
        mpp_err_f("ioctl MPP_IOC_CFG_V1 failed ret %d errno %d %s\n",
                  ret, errno, strerror(errno));
        ret = errno;
    }

    mpp_dev_dbg_func("leave %p\n", ctx);
    return ret;
}

MPP_RET mpp_device_send_extra_info(MppDevCtx ctx, RegExtraInfo *info)
{
    MPP_RET ret = MPP_OK;

    if (NULL == ctx || NULL == info) {
        mpp_err_f("found NULL input ctx %p size %d\n", ctx, info);
        return MPP_ERR_NULL_PTR;
    }

    mpp_dev_dbg_func("enter %p info %p\n", ctx, info);

    /* 1. When extra info is invalid do NOT send */
    if (info->magic != EXTRA_INFO_MAGIC || !info->count)
        return ret;

    MppDevCtxImpl *p = (MppDevCtxImpl *)ctx;

    /* 2. When on old kernel do NOT send */
    if (p->ioctl_version == IOCTL_VCODEC_SERVICE)
        return ret;

    if (p->ioctl_version == IOCTL_MPP_SERVICE_V1) {
        MppDevReqV1 dev_req;

        memset(&dev_req, 0, sizeof(dev_req));
        dev_req.cmd = MPP_CMD_SET_REG_ADDR_OFFSET;
        dev_req.flag = 0;
        dev_req.offset = 0;
        dev_req.size = info->count * sizeof(info->patchs[0]);
        dev_req.data = (void *)&info->patchs[0];
        ret = mpp_device_add_request(ctx, &dev_req);
        if (ret)
            mpp_err_f("mpp_device_send_extra_info failed ret %d\n", ret);
    }

    mpp_dev_dbg_func("leave %p ret %d\n", ctx, ret);

    return ret;
}

MPP_RET mpp_device_send_reg(MppDevCtx ctx, RK_U32 *regs, RK_U32 nregs)
{
    int ret = 0;
    MppDevCtxImpl *p;

    mpp_dev_dbg_func("enter %p reg %p nregs %d\n", ctx, regs, nregs);

    if (NULL == ctx || NULL == regs) {
        mpp_err_f("found NULL input ctx %p regs %p\n", ctx, regs);
        return MPP_ERR_NULL_PTR;
    }

    p = (MppDevCtxImpl *)ctx;

    if (mpp_device_debug & MPP_DEVICE_DBG_REG) {
        RK_U32 i;

        for (i = 0; i < nregs; i++) {
            mpp_log("set reg[%03d]: %08x\n", i, regs[i]);
        }
    }

    if (mpp_device_debug & MPP_DEVICE_DBG_TIME) {
        p->time_start[p->idx_send++] = mpp_time();
        if (p->idx_send >= MAX_TIME_RECORD)
            p->idx_send = 0;
    }

    if (p->ioctl_version > 0) {
        MppDevReqV1 dev_req;

        memset(&dev_req, 0, sizeof(dev_req));
        dev_req.cmd = MPP_CMD_SET_REG_WRITE;
        dev_req.flag = 0;
        dev_req.offset = 0;
        dev_req.size = nregs * sizeof(RK_U32);
        dev_req.data = (void*)regs;
        mpp_device_add_request(ctx, &dev_req);

        memset(&dev_req, 0, sizeof(dev_req));
        dev_req.cmd = MPP_CMD_SET_REG_READ;
        dev_req.flag = 0;
        dev_req.offset = 0;
        dev_req.size = nregs * sizeof(RK_U32);
        dev_req.data = (void*)regs;
        mpp_device_add_request(ctx, &dev_req);

        ret = mpp_device_send_request(ctx);
    } else {
        MppReq req;

        req.req     = regs;
        req.size    = nregs * sizeof(RK_U32);
        ret = (RK_S32)ioctl(p->vpu_fd, VPU_IOC_SET_REG, &req);
    }

    if (ret) {
        mpp_err_f("ioctl VPU_IOC_SET_REG failed ret %d errno %d %s\n",
                  ret, errno, strerror(errno));
        ret = errno;
    }

    mpp_dev_dbg_func("leave %p ret %d\n", ctx, ret);
    return (ret ? MPP_NOK : MPP_OK);
}

MPP_RET mpp_device_wait_reg(MppDevCtx ctx, RK_U32 *regs, RK_U32 nregs)
{
    int ret = 0;
    MppReq req;
    MppDevCtxImpl *p;

    mpp_dev_dbg_func("enter %p regs %p nregs %d\n", ctx, regs, nregs);

    if (NULL == ctx || NULL == regs) {
        mpp_err_f("found NULL input ctx %p regs %p\n", ctx, regs);
        return MPP_ERR_NULL_PTR;
    }

    p = (MppDevCtxImpl *)ctx;

    if (p->ioctl_version > 0) {
        MppDevReqV1 dev_req;

        memset(&dev_req, 0, sizeof(dev_req));
        dev_req.cmd = MPP_CMD_POLL_HW_FINISH;
        dev_req.flag |= MPP_FLAGS_LAST_MSG;
        ret = mpp_device_send_single_request(ctx, &dev_req);
    } else {
        memset(&req, 0, sizeof(req));
        req.req     = regs;
        req.size    =  nregs * sizeof(RK_U32);
        ret = (RK_S32)ioctl(p->vpu_fd, VPU_IOC_GET_REG, &req);
    }

    if (mpp_device_debug & MPP_DEVICE_DBG_TIME) {
        RK_S32 idx = p->idx_wait;
        p->time_end[idx] = mpp_time();

        mpp_log("task %d time %.2f ms\n", p->idx_total,
                (p->time_end[idx] - p->time_start[idx]) / 1000.0);

        p->idx_total++;
        if (++p->idx_wait >= MAX_TIME_RECORD)
            p->idx_wait = 0;
    }

    if (mpp_device_debug & MPP_DEVICE_DBG_REG) {
        RK_U32 i;

        for (i = 0; i < nregs; i++) {
            mpp_log("get reg[%03d]: %08x\n", i, regs[i]);
        }
    }

    mpp_dev_dbg_func("leave %p ret %d\n", ctx, ret);
    return (ret ? MPP_NOK : MPP_OK);
}

MPP_RET mpp_device_send_reg_with_id(MppDevCtx ctx, RK_S32 id, void *param,
                                    RK_S32 size)
{
    MPP_RET ret = MPP_NOK;
    MppDevCtxImpl *p;

    mpp_dev_dbg_func("enter %p id %d param %p size %d\n", ctx, id, param, size);

    if (NULL == ctx || NULL == param) {
        mpp_err_f("found NULL input ctx %p param %p\n", ctx, param);
        return MPP_ERR_NULL_PTR;
    }

    p = (MppDevCtxImpl *)ctx;

    if (p->ioctl_version > 0) {
        mpp_err("not support now\n");
        return MPP_ERR_UNKNOW;
    } else {
        ret = (RK_S32)ioctl(p->vpu_fd, VPU_IOC_WRITE(id, size), param);
    }

    if (ret) {
        mpp_err_f("ioctl failed ret %d errno %d %s\n",
                  ret, errno, strerror(errno));
        ret = errno;
    }

    mpp_dev_dbg_func("leave %p ret %d\n", ctx, ret);

    return ret;
}

RK_S32 mpp_device_control(MppDevCtx ctx, MppDevCmd cmd, void* param)
{
    MppDevCtxImpl *p;

    if (NULL == ctx || NULL == param) {
        mpp_err_f("found NULL input ctx %p param %p\n", ctx, param);
        return MPP_ERR_NULL_PTR;
    }

    p = (MppDevCtxImpl *)ctx;

    switch (cmd) {
    case MPP_DEV_GET_MMU_STATUS : {
        p->mmu_status = 1;
        *((RK_U32 *)param) = p->mmu_status;
    } break;
    case MPP_DEV_ENABLE_POSTPROCCESS : {
        p->pp_enable = 1;
    } break;
    case MPP_DEV_SET_HARD_PLATFORM : {
        p->platform = *((RK_U32 *)param);
    } break;
    default : {
    } break;
    }

    return 0;
}

void mpp_device_patch_init(RegExtraInfo *extra)
{
    extra->magic = EXTRA_INFO_MAGIC;
    extra->count = 0;
}

void mpp_device_patch_add(RK_U32 *reg, RegExtraInfo *extra, RK_U32 reg_idx, RK_U32 offset)
{
    if (offset < SZ_4M) {
        reg[reg_idx] += (offset << 10);
        return ;
    }

    if (extra->count >= MPX_PATCH_NUM) {
        mpp_err_f("too much %d patch count larger than %d\n",
                  extra->count, MPX_PATCH_NUM);

        return ;
    }

    RegPatchInfo *info = &extra->patchs[extra->count++];
    info->reg_idx = reg_idx;
    info->offset = offset;
}

RK_S32 mpp_device_patch_is_valid(RegExtraInfo *extra)
{
    return extra->magic == EXTRA_INFO_MAGIC && extra->count;
}
