/*******************************************************************************
* Copyright 2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef CPU_JIT_AVX512_CORE_U8S8S32X_1X1_CONVOLUTION_HPP
#define CPU_JIT_AVX512_CORE_U8S8S32X_1X1_CONVOLUTION_HPP

#include "c_types_map.hpp"
#include "cpu_convolution_pd.hpp"
#include "cpu_engine.hpp"
#include "cpu_reducer.hpp"
#include "mkldnn_thread.hpp"
#include "utils.hpp"

#include "jit_uni_1x1_conv_utils.hpp"
#include "jit_avx512_core_u8s8s32x_1x1_conv_kernel.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

template <bool with_relu, impl::data_type_t dst_type>
struct _jit_avx512_core_u8s8s32x_1x1_convolution_fwd_t : public cpu_primitive_t {
    struct pd_t: public _cpu_convolution_fwd_pd_t<with_relu> {
        pd_t(engine_t *engine,
                const typename pd_t::base_desc_t *adesc,
                const primitive_attr_t *attr,
                const typename pd_t::base_class *hint_fwd_pd)
            : _cpu_convolution_fwd_pd_t<with_relu>(engine, adesc, attr,
                    hint_fwd_pd)
            , jcp_(), rtus_() {}

        DECLARE_COMMON_PD_T(
                JIT_IMPL_NAME_HELPER("jit_1x1:", avx512_core, ""),
                _jit_avx512_core_u8s8s32x_1x1_convolution_fwd_t<with_relu,
                dst_type>);

        virtual status_t init() override {
            using namespace prop_kind;
            using namespace utils;
            assert(this->engine()->kind() == engine_kind::cpu);
            bool ok = true
                && this->set_default_params() == status::success
                && utils::one_of(this->cdesc_().prop_kind, forward_training,
                        forward_inference)
                && this->cdesc_().alg_kind == alg_kind::convolution_direct
                && this->cdesc_().src_desc.data_type == data_type::u8
                && this->cdesc_().dst_desc.data_type == dst_type
                && this->cdesc_().weights_desc.data_type == data_type::s8
                && utils::implication(this->with_bias(), utils::one_of(
                            this->cdesc_().bias_desc.data_type, data_type::f32,
                            data_type::s32, data_type::s8, data_type::u8))
                && this->cdesc_().accum_data_type == data_type::s32;

            if (!ok) return status::unimplemented;

            const convolution_desc_t *conv_d = &this->cdesc_();
            const memory_desc_t *src_d = this->src_pd_.desc();
            rtus_prepare(this, conv_d, src_d, this->dst_pd_.desc());
            return jit_avx512_core_u8s8s32x_1x1_conv_kernel::init_conf(jcp_,
                    *conv_d, *src_d, *this->weights_pd_.desc(),
                    *this->dst_pd_.desc(), *this->bias_pd_.desc(), *this->attr(),
                    with_relu, this->negative_slope(),
                    omp_get_max_threads(), rtus_.reduce_src_);
        }

        jit_1x1_conv_conf_t jcp_;
        struct reduce_to_unit_stride_t {
            convolution_desc_t conv_d_;
            bool reduce_src_;
        } rtus_;

      protected:
        virtual status_t set_default_params() override {
            using namespace memory_format;
            if (this->src_pd_.desc()->format == any)
                CHECK(this->src_pd_.set_format(nhwc));
            if (this->dst_pd_.desc()->format == any)
                CHECK(this->dst_pd_.set_format(nhwc));
            if (this->weights_pd_.desc()->format == any)
                CHECK(this->weights_pd_.set_format(this->with_groups()
                                        ? gOIhw4i16o4i : OIhw4i16o4i));
            if (this->bias_pd_.desc()->format == any)
                CHECK(this->bias_pd_.set_format(x));
            return status::success;
        }
    };

    template <cpu_isa_t isa, typename conv_t>
    friend void init_rtus_driver(conv_t *self);
    _jit_avx512_core_u8s8s32x_1x1_convolution_fwd_t(const pd_t *pd,
                                          const input_vector &inputs,
                                          const output_vector &outputs)
        : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd)
        , kernel_(nullptr), rtus_driver_(nullptr), ws_per_thread_(0)
        , scratch_(nullptr)
    {
        kernel_ = new jit_avx512_core_u8s8s32x_1x1_conv_kernel(conf_.jcp_,
                    *conf_.attr());

        ws_size_ = conf_.jcp_.mb * conf_.jcp_.oc * conf_.jcp_.ow * conf_.jcp_.oh;
        ws_ = (acc_data_t *)malloc(ws_size_ * sizeof(acc_data_t), 64);
        init_rtus_driver<avx512_common>(this);
    }
    ~_jit_avx512_core_u8s8s32x_1x1_convolution_fwd_t() {
        delete kernel_;
        delete rtus_driver_;
        free(ws_);
        free(scratch_);
    }

    typedef typename prec_traits<data_type::u8>::type src_data_t;
    typedef typename prec_traits<data_type::s8>::type wei_data_t;
    typedef typename prec_traits<dst_type>::type dst_data_t;
    typedef typename prec_traits<data_type::s32>::type acc_data_t;

    virtual void execute(event_t *e) {
        execute_forward();
        e->set_state(event_t::ready);
    }

  private:
    void execute_forward();
    pd_t conf_;
    jit_avx512_core_u8s8s32x_1x1_conv_kernel *kernel_;

    rtus_driver_t<avx512_common> *rtus_driver_;
    size_t ws_per_thread_;
    src_data_t *scratch_;
    acc_data_t *ws_;
    size_t ws_size_;
};

template <impl::data_type_t dst_type>
using jit_avx512_core_u8s8s32x_1x1_convolution_fwd_t =
    _jit_avx512_core_u8s8s32x_1x1_convolution_fwd_t<false, dst_type>;

template <impl::data_type_t dst_type>
using jit_avx512_core_u8s8s32x_1x1_convolution_relu_t =
    _jit_avx512_core_u8s8s32x_1x1_convolution_fwd_t<true, dst_type>;

}
}
}

#endif
