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

#ifndef __MPP_CODEC_INFO_H__
#define __MPP_CODEC_INFO_H__

#include "rk_type.h"
#include "mpp_err.h"

typedef enum RkvEncInfoType_e {
    RKVE_INFO_BASE              = 0,
    RKVE_INFO_WIDTH,
    RKVE_INFO_HEIGHT,
    RKVE_INFO_FORMAT,
    RKVE_INFO_FPS_IN,
    RKVE_INFO_FPS_OUT,
    RKVE_INFO_RC_MODE,
    RKVE_INFO_BITRATE,

    RKVE_INFO_GOP_SIZE,
    RKVE_INFO_FPS_CALC,
    RKVE_INFO_PROFILE,

    RKVE_INFO_BUTT,
} RkvEncInfoType;

enum ENC_INFO_FLAGS {
    RKVE_INFO_FLAG_NULL     = 0,
    RKVE_INFO_FLAG_NUMBER,
    RKVE_INFO_FLAG_STRING,

    RKVE_INFO_FLAG_BUTT,
};

typedef struct RkvCodecInfoElem_t {
    RK_U32 type;
    RK_U32 flag;
    RK_U64 data;
} RkvCodecInfoElem;

typedef struct RkvCodecInfo_t {
    RkvCodecInfoElem elem_last[RKVE_INFO_BUTT];
    RkvCodecInfoElem elem_set[RKVE_INFO_BUTT];

    RK_S32 cnt;
} RkvCodecInfo;


#ifdef __cplusplus
extern "C" {
#endif

MPP_RET mpp_set_codec_info_elem(RkvCodecInfo *info, RK_U32 type, RK_U32 flag, RK_U64 data);
MPP_RET mpp_get_rc_mode_codec_info_data(RK_S32 rc_mode, RK_U64 *data);

#ifdef __cplusplus
}
#endif

#endif /*__MPP_CODEC_INFO_H__*/

