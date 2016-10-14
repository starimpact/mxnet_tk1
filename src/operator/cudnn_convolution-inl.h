/*!
 * Copyright (c) 2015 by Contributors
 * \file cudnn_convolution-inl.h
 * \brief
 * \author Bing Xu
*/
#ifndef MXNET_OPERATOR_CUDNN_CONVOLUTION_INL_H_
#define MXNET_OPERATOR_CUDNN_CONVOLUTION_INL_H_

#include <algorithm>
#include <vector>
#include "./convolution-inl.h"
namespace mxnet {
namespace op {
#if defined(__CUDACC__) && MXNET_USE_CUDNN == 1
template<typename DType>
class CuDNNConvolutionOp : public Operator {
 public:
  explicit CuDNNConvolutionOp(ConvolutionParam param) {
    this->param_ = param;
    // convert MB to words
    param_.workspace = (param_.workspace << 20) / sizeof(DType);
    init_cudnn_ = false;
    // TODO(xxx): fp16
    dtype_ = mshadow::DataType<DType>::kCudnnFlag;
  }

  ~CuDNNConvolutionOp() {
    if (init_cudnn_) {
      CHECK_EQ(cudnnDestroyTensorDescriptor(in_desc_), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnDestroyTensorDescriptor(out_desc_), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnDestroyTensorDescriptor(bias_desc_), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnDestroyFilterDescriptor(filter_desc_), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnDestroyConvolutionDescriptor(conv_desc_), CUDNN_STATUS_SUCCESS);
    }
  }

  virtual void Forward(const OpContext &ctx,
                       const std::vector<TBlob> &in_data,
                       const std::vector<OpReqType> &req,
                       const std::vector<TBlob> &out_data,
                       const std::vector<TBlob> &aux_args) {
    using namespace mshadow;
    size_t expected = param_.no_bias ? 2 : 3;
    CHECK_EQ(in_data.size(), expected);
    CHECK_EQ(out_data.size(), 1);
    Stream<gpu> *s = ctx.get_stream<gpu>();
    Tensor<gpu, 4, DType> data = in_data[conv::kData].get<gpu, 4, DType>(s);
    Tensor<gpu, 4, DType> wmat = in_data[conv::kWeight].get<gpu, 4, DType>(s);
    Tensor<gpu, 4, DType> out = out_data[conv::kOut].get<gpu, 4, DType>(s);
    CHECK_EQ(data.CheckContiguous(), true);
    CHECK_EQ(wmat.CheckContiguous(), true);
    CHECK_EQ(out.CheckContiguous(), true);
    if (!init_cudnn_) {
      Init(s, in_data, out_data);
    }
    Tensor<gpu, 1, DType> workspace =
        ctx.requested[conv::kTempSpace].get_space_typed<gpu, 1, DType>(
                                 mshadow::Shape1(forward_workspace_), s);
    for (uint32_t g = 0; g < param_.num_group; ++g) {
      typename DataType<DType>::ScaleType alpha = 1.0f;
      typename DataType<DType>::ScaleType beta = 0.0f;
      CHECK_EQ(cudnnConvolutionForward(s->dnn_handle_,
                                       &alpha,
                                       in_desc_,
                                       data.dptr_ + data_offset_ * g,
                                       filter_desc_,
                                       wmat.dptr_ + weight_offset_ * g,
                                       conv_desc_,
                                       algo_,
                                       workspace.dptr_,
                                       forward_workspace_byte_,
                                       &beta,
                                       out_desc_,
                                       out.dptr_ + out_offset_ * g), CUDNN_STATUS_SUCCESS);
      if (!param_.no_bias) {
        beta = 1.0f;
        Tensor<gpu, 1, DType> bias = in_data[conv::kBias].get<gpu, 1, DType>(s);
#if CUDNN_MAJOR >= 4
        CHECK_EQ(cudnnAddTensor(s->dnn_handle_,
                                &alpha,
                                bias_desc_,
                                bias.dptr_ + bias_offset_ * g,
                                &beta,
                                out_desc_,
                                out.dptr_ + out_offset_ * g), CUDNN_STATUS_SUCCESS);
#endif
#if CUDNN_MAJOR == 3 || CUDNN_VERSION == 2000
        CHECK_EQ(cudnnAddTensor(s->dnn_handle_,
                                CUDNN_ADD_SAME_C,
                                &alpha,
                                bias_desc_,
                                bias.dptr_ + bias_offset_ * g,
                                &beta,
                                out_desc_,
                                out.dptr_ + out_offset_ * g), CUDNN_STATUS_SUCCESS);
#endif
      }
    }
  }

  virtual void Backward(const OpContext &ctx,
                        const std::vector<TBlob> &out_grad,
                        const std::vector<TBlob> &in_data,
                        const std::vector<TBlob> &out_data,
                        const std::vector<OpReqType> &req,
                        const std::vector<TBlob> &in_grad,
                        const std::vector<TBlob> &aux_args) {

  }

 private:
  inline void Init(mshadow::Stream<gpu> *s,
                   const std::vector<TBlob> &in_data,
                   const std::vector<TBlob> &out_data) {
    using namespace mshadow;
    size_t expected = param_.no_bias ? 2 : 3;
    #if CUDNN_MAJOR == 5
    format_ = CUDNN_TENSOR_NCHW;
    #endif
    CHECK_EQ(in_data.size(), expected);
    CHECK_EQ(out_data.size(), 1);
    if (!init_cudnn_) {
      init_cudnn_ = true;
      size_t workspace_byte = static_cast<size_t>(param_.workspace * sizeof(DType));
      Tensor<gpu, 4, DType> data = in_data[conv::kData].get<gpu, 4, DType>(s);
      Tensor<gpu, 4, DType> out = out_data[conv::kOut].get<gpu, 4, DType>(s);
      data_offset_ = data.shape_[1] / param_.num_group * data.shape_[2] * data.shape_[3];
      out_offset_ = out.shape_[1] /param_.num_group * out.shape_[2] * out.shape_[3];
      weight_offset_ = param_.num_filter / param_.num_group * data.shape_[1] / param_.num_group
                       * param_.kernel[0] * param_.kernel[1];
      CHECK_EQ(cudnnCreateTensorDescriptor(&in_desc_), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnCreateTensorDescriptor(&out_desc_), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnCreateTensorDescriptor(&bias_desc_), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnCreateFilterDescriptor(&filter_desc_), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnCreateConvolutionDescriptor(&conv_desc_), CUDNN_STATUS_SUCCESS);
      #if CUDNN_MAJOR == 5
      CHECK_EQ(cudnnSetFilter4dDescriptor(filter_desc_,
                                          dtype_,
                                          format_,
                                          param_.num_filter / param_.num_group,
                                          data.shape_[1] / param_.num_group,
                                          param_.kernel[0],
                                          param_.kernel[1]), CUDNN_STATUS_SUCCESS);
      #else
      CHECK_EQ(cudnnSetFilter4dDescriptor(filter_desc_,
                                          dtype_,
                                          param_.num_filter / param_.num_group,
                                          data.shape_[1] / param_.num_group,
                                          param_.kernel[0],
                                          param_.kernel[1]), CUDNN_STATUS_SUCCESS);
      #endif
      CHECK_EQ(cudnnSetConvolution2dDescriptor(conv_desc_,
                                               param_.pad[0],
                                               param_.pad[1],
                                               param_.stride[0],
                                               param_.stride[1],
                                               1,
                                               1,
                                               CUDNN_CROSS_CORRELATION), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnSetTensor4dDescriptorEx(in_desc_,
                                            dtype_,
                                            data.shape_[0],
                                            data.shape_[1] / param_.num_group,
                                            data.shape_[2],
                                            data.shape_[3],
                                            data.shape_[1] * data.shape_[2] * data.shape_[3],
                                            data.shape_[2] * data.shape_[3],
                                            data.shape_[3],
                                            1), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnSetTensor4dDescriptorEx(out_desc_,
                                            dtype_,
                                            out.shape_[0],
                                            out.shape_[1] / param_.num_group,
                                            out.shape_[2],
                                            out.shape_[3],
                                            out.shape_[1] * out.shape_[2] * out.shape_[3],
                                            out.shape_[2] * out.shape_[3],
                                            out.shape_[3],
                                            1), CUDNN_STATUS_SUCCESS);
      if (!param_.no_bias) {
        Tensor<gpu, 1, DType> bias = in_data[conv::kBias].get<gpu, 1, DType>(s);
        bias_offset_ = bias.shape_[0] / param_.num_group;
        CHECK_EQ(cudnnSetTensor4dDescriptor(bias_desc_,
                                            CUDNN_TENSOR_NCHW,
                                            dtype_,
                                            1,
                                            bias.shape_[0] / param_.num_group,
                                            1,
                                            1), CUDNN_STATUS_SUCCESS);
      }
      CHECK_EQ(s->dnn_handle_ownership_, mshadow::Stream<gpu>::OwnHandle);
      CHECK_EQ(cudnnGetConvolutionForwardAlgorithm(s->dnn_handle_,
               in_desc_,
               filter_desc_,
               conv_desc_,
               out_desc_,
               CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT,
               workspace_byte,
               &algo_), CUDNN_STATUS_SUCCESS);
      CHECK_EQ(cudnnGetConvolutionForwardWorkspaceSize(s->dnn_handle_,
               in_desc_,
               filter_desc_,
               conv_desc_,
               out_desc_,
               algo_,
               &forward_workspace_byte_), CUDNN_STATUS_SUCCESS);
      forward_workspace_ = forward_workspace_byte_ / sizeof(DType) + 1;
    }
  }

  bool init_cudnn_;
  size_t forward_workspace_;
  size_t forward_workspace_byte_;
  size_t data_offset_;
  size_t out_offset_;
  size_t weight_offset_;
  size_t bias_offset_;
  cudnnDataType_t dtype_;
  cudnnTensorDescriptor_t in_desc_;
  cudnnTensorDescriptor_t out_desc_;
  cudnnTensorDescriptor_t bias_desc_;
  cudnnFilterDescriptor_t filter_desc_;
  cudnnConvolutionDescriptor_t conv_desc_;
  cudnnConvolutionFwdAlgo_t algo_;
  #if CUDNN_MAJOR == 5
  cudnnTensorFormat_t format_;
  #endif
  ConvolutionParam param_;
};
#endif  // __CUDACC__ && CUDNN
}  // namespace op
}  // namespace mxnet

#endif  // MXNET_OPERATOR_CUDNN_CONVOLUTION_INL_H_
