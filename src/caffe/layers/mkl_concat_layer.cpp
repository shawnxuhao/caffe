#if defined(MKL2017_SUPPORTED)
#include <vector>

#include "caffe/layer.hpp"
#include "caffe/layers/mkl_layers.hpp"

namespace caffe {

template <typename Dtype> MKLConcatLayer<Dtype>::~MKLConcatLayer() {
  dnnDelete<Dtype>(concatFwd_);
  dnnDelete<Dtype>(concatBwd_);
}

template <typename Dtype>
void MKLConcatLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
  const vector<Blob<Dtype>*>& top) {
  size_t dim_src = bottom[0]->shape().size();
  size_t dim_dst = dim_src;

  num_concats_ = bottom.size();
  channels_ = 0;

  for (size_t i = 1; i < num_concats_; ++i) {
    CHECK_EQ(bottom[0]->num(), bottom[i]->num());
    CHECK_EQ(bottom[0]->height(), bottom[i]->height());
    CHECK_EQ(bottom[0]->width(), bottom[i]->width());
  }

  split_channels_ = new size_t[num_concats_];
  for (size_t i = 0; i < num_concats_; ++i) {
    CHECK_EQ(dim_src, bottom[i]->shape().size());

    fwd_bottom_data_.push_back(shared_ptr<MKLData<Dtype> >(new MKLData<Dtype>));
    bwd_bottom_diff_.push_back(shared_ptr<MKLDiff<Dtype> >(new MKLDiff<Dtype>));
    fwd_bottom_data_[i]->name = "fwd_bottom_data_[i]";
    bwd_bottom_diff_[i]->name = "bwd_bottom_data[i]";

    // TODO: should be a helper function
    size_t sizes_src[dim_src], strides_src[dim_src];
    for (size_t d = 0; d < dim_src; ++d) {
        sizes_src[d] = bottom[i]->shape()[dim_src - d - 1];
        strides_src[d] = (d == 0) ? 1 : strides_src[d - 1] * sizes_src[d - 1];
    }

    split_channels_[i] = bottom[i]->channels();
    channels_ += split_channels_[i];
    fwd_bottom_data_[i]->create_user_layout(dim_src, sizes_src, strides_src);
    bwd_bottom_diff_[i]->create_user_layout(dim_src, sizes_src, strides_src);
  }

  // XXX: almost the same computations as above for src
  size_t sizes_dst[dim_dst], strides_dst[dim_dst];
  for (size_t d = 0; d < dim_dst; ++d) {
    if (d == 2)
      sizes_dst[d] = channels_;
    else
      sizes_dst[d] = bottom[0]->shape()[dim_dst - 1 - d];
    strides_dst[d] = (d == 0) ? 1 : strides_dst[d - 1] * sizes_dst[d - 1];
  }
  bwd_top_diff_->create_user_layout(dim_dst, sizes_dst, strides_dst);
  fwd_top_data_->create_user_layout(dim_dst, sizes_dst, strides_dst);

  concatFwd_ = NULL;
  concatBwd_ = NULL;
}

template <typename Dtype>
void MKLConcatLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
  const vector<Blob<Dtype>*>& top) {
  num_ = bottom[0]->num();
  height_ = bottom[0]->height();
  width_ = bottom[0]->width();

  top[0]->Reshape(num_, channels_, height_, width_);
}

template <typename Dtype>
void MKLConcatLayer<Dtype>::Forward_cpu(const vector <Blob<Dtype>*>& bottom,
  const vector <Blob<Dtype>*>& top) {
  dnnError_t e;
  vector<void*> bottom_data;
  bool isFirstPass = (concatFwd_ == NULL);
  dnnLayout_t *layouts = NULL;
  bool *isBottomDataFilled = NULL;
  if (isFirstPass) {
      layouts = new dnnLayout_t[num_concats_];
      isBottomDataFilled = new bool[num_concats_]();
  }

  for (size_t n = 0; n < num_concats_; n++) {
    bottom_data.push_back(reinterpret_cast<void *>(
      const_cast<Dtype*>(bottom[n]->prv_data())));

    if (bottom_data[n] == NULL) {
      bottom_data[n] =
        reinterpret_cast<void *>(const_cast<Dtype*>(bottom[n]->cpu_data()));
      if (isFirstPass) {
        layouts[n] = fwd_bottom_data_[n]->layout_usr;
        isBottomDataFilled[n] = true;
      }
    } else if (isFirstPass) {
      CHECK((bottom[n]->get_prv_data_descriptor())->get_descr_type() ==
        PrvMemDescr::PRV_DESCR_MKL2017);
      shared_ptr<MKLData<Dtype> > mem_descr =
        boost::static_pointer_cast<MKLData<Dtype> >(
          bottom[n]->get_prv_data_descriptor());
      CHECK(mem_descr != NULL);

      fwd_bottom_data_[n] = mem_descr;
      layouts[n] = mem_descr->layout_int;
    }
  }

  if (isFirstPass) {
    e = dnnConcatCreate<Dtype>(&concatFwd_, NULL, num_concats_, layouts);
    CHECK_EQ(e, E_SUCCESS);

    fwd_top_data_->create_internal_layout(concatFwd_, dnnResourceDst);
    bwd_top_diff_->create_internal_layout(concatFwd_, dnnResourceDst);

    e = dnnSplitCreate<Dtype>(&concatBwd_, NULL, num_concats_,
      bwd_top_diff_->layout_int, split_channels_);
    CHECK_EQ(e, E_SUCCESS);

    for (size_t n = 0; n < num_concats_; ++n) {
      if (isBottomDataFilled[n]) continue;

      fwd_bottom_data_[n]->create_internal_layout(concatFwd_,
          (dnnResourceType_t)(dnnResourceMultipleSrc + n));
      bwd_bottom_diff_[n]->create_internal_layout(concatBwd_,
          (dnnResourceType_t)(dnnResourceMultipleDst + n));
    }
  }

  delete[] layouts;
  delete[] isBottomDataFilled;

  void *concat_res[dnnResourceNumber];
  for (int n = 0; n < num_concats_; ++n) {
    concat_res[dnnResourceMultipleSrc + n]
      = reinterpret_cast<void*>(bottom_data[n]);
  }

  if (fwd_top_data_->conversion_needed()) {
    top[0]->set_prv_data_descriptor(fwd_top_data_);
    concat_res[dnnResourceDst] =
      reinterpret_cast<void*>(top[0]->mutable_prv_data());
  } else {
    concat_res[dnnResourceDst] =
      reinterpret_cast<void*>(top[0]->mutable_cpu_data());
  }

  e = dnnExecute<Dtype>(concatFwd_, concat_res);
  CHECK_EQ(e, E_SUCCESS);
}

template <typename Dtype>
void MKLConcatLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
  const vector<bool>& propagate_down,
  const vector <Blob<Dtype>*>& bottom) {
  int need_bwd = 0;
  for (size_t n = 0; n < num_concats_; n++) {
    need_bwd += propagate_down[n];
  }
  if (!need_bwd) {
    return;
  }

  dnnError_t e;
  void *concat_res[dnnResourceNumber];

  concat_res[dnnResourceSrc] = bwd_top_diff_->get_converted_prv(top[0], true);

  for (size_t i = 0; i < num_concats_; ++i) {
    if (bwd_bottom_diff_[i]->conversion_needed()) {
      bottom[i]->set_prv_diff_descriptor(bwd_bottom_diff_[i]);
      concat_res[dnnResourceMultipleDst + i] = bottom[i]->mutable_prv_diff();
    } else {
      concat_res[dnnResourceMultipleDst + i] = bottom[i]->mutable_cpu_diff();
    }
  }

  e = dnnExecute<Dtype>(concatBwd_, concat_res);
  CHECK_EQ(e, E_SUCCESS);
}

#ifdef CPU_ONLY
STUB_GPU(MKLConcatLayer);
#else
template <typename Dtype>
void MKLConcatLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {NOT_IMPLEMENTED;}
template <typename Dtype>
void MKLConcatLayer<Dtype>::Backward_gpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom)
  {NOT_IMPLEMENTED;}
#endif

INSTANTIATE_CLASS(MKLConcatLayer);
}  // namespace caffe
#endif  // #if defined(MKL2017_SUPPORTED)
