﻿// Copyright (C) 2018-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <gtest/gtest.h>
#include <ie_core.hpp>

#include "tests_common.hpp"
#include "single_layer_common.hpp"
#include "ir_gen_helper.hpp"
#include <utility>
#include <string>
#include <memory>
#include <vector>

using namespace InferenceEngine;
using namespace ::testing;

struct gather_tree_test_params {
    SizeVector           in_out_shape;
    std::vector<int32_t> step_idx;
    std::vector<int32_t> parent_idx;
    std::vector<int32_t> max_seq_len;
    std::vector<int32_t> end_token;
    std::vector<int32_t> reference;
    std::string device_name;
};


template <typename data_t>
void ref_gather_tree(
    InferenceEngine::TBlob<data_t> &step_idx,
    InferenceEngine::TBlob<int32_t> &parent_idx,
    InferenceEngine::TBlob<int32_t> &max_seq_len,
    InferenceEngine::TBlob<data_t> &end_token,
    InferenceEngine::TBlob<data_t> &dst
) {
    const data_t *step_idxPtr = step_idx.data();
    const int32_t *parent_idxPtr = parent_idx.data();
    const int32_t *max_seq_lenPtr = max_seq_len.data();
    const data_t *end_tokenPtr = end_token.data();
    data_t *final_idxPtr = dst.data();

    SizeVector step_idx_dims = step_idx.getTensorDesc().getDims();
    SizeVector parent_idx_dims = parent_idx.getTensorDesc().getDims();
    SizeVector max_seq_len_dims = max_seq_len.getTensorDesc().getDims();
    SizeVector final_idx_dims = dst.getTensorDesc().getDims();
    int32_t max_time = step_idx_dims[0];
    int32_t batch_size = step_idx_dims[1];
    int32_t beam_width = step_idx_dims[2];

    if (max_time != parent_idx_dims[0] || max_time != final_idx_dims[0] ||
        batch_size != parent_idx_dims[1] || batch_size != final_idx_dims[1] || batch_size != max_seq_len_dims[0] ||
        beam_width != parent_idx_dims[2] || beam_width != final_idx_dims[2]) {
        FAIL() << " Input/Output tensors dimensions mismatch";
        return;
    }

    for (int32_t time, batch = 0; batch < batch_size; batch++) {
        for (int32_t beam = 0; beam < beam_width; beam++) {
            int32_t max_sequence_in_beam = (std::min)(max_time, max_seq_lenPtr[batch]);
            if (max_sequence_in_beam <= 0)
                continue;

            for (time = (max_time - 1); time >= max_sequence_in_beam; time--)
                final_idxPtr[(time * batch_size + batch) * beam_width + beam] = (*end_tokenPtr);

            for (int32_t parent = beam; time >= 0; time--) {
                if (parent < 0 || parent >= beam_width) {
                    FAIL() << " Wrong parent index";
                    return;
                }

                int32_t idx = (time * batch_size + batch) * beam_width;
                final_idxPtr[idx + beam] = step_idxPtr[idx + parent];
                parent = parent_idxPtr[idx + parent];
            }

            bool finished = false;
            data_t *final = &final_idxPtr[batch * beam_width + beam];

            for (time = 0; time < max_sequence_in_beam; time++, final += (batch_size * beam_width)) {
                if (finished)
                    (*final) = (*end_tokenPtr);
                else if ((*final) == (*end_tokenPtr))
                    finished = true;
            }
        }
    }
}

class GatherTreeTests : public TestsCommon, public WithParamInterface<gather_tree_test_params> {
    std::string model_t = R"V0G0N(
<net Name="GatherTree_net" version="2" precision="FP32" batch="1">
    <layers>
        <layer name="step_idx" type="Input" precision="I32" id="1">
            <output>
                <port id="1">
                    _IN_OUT_
                </port>
            </output>
        </layer>
        <layer name="parent_idx" type="Input" precision="I32" id="2">
            <output>
                <port id="2">
                    _IN_OUT_
                </port>
            </output>
        </layer>
        <layer name="max_seq_len" type="Input" precision="I32" id="3">
            <output>
                <port id="3">
                    <dim>_IN2_</dim>
                </port>
            </output>
        </layer>
        <layer name="end_token" type="Input" precision="I32" id="4">
            <output>
                <port id="4">
                    <dim>1</dim>
                </port>
            </output>
        </layer>
        <layer name="output" id="2" type="GatherTree" precision="I32">
            <data/>
            <input>
                <port id="1">
                    _IN_OUT_
                </port>
                <port id="2">
                    _IN_OUT_
                </port>
                <port id="3">
                    <dim>_IN2_</dim>
                </port>
                <port id="4">
                    <dim>1</dim>
                </port>
            </input>
            <output>
                <port id="5">
                    _IN_OUT_
                </port>
            </output>
        </layer>
    </layers>
    <edges>
        <edge from-layer="1" from-port="1" to-layer="2" to-port="1"/>
        <edge from-layer="2" from-port="2" to-layer="2" to-port="2"/>
        <edge from-layer="3" from-port="3" to-layer="2" to-port="3"/>
        <edge from-layer="4" from-port="4" to-layer="2" to-port="4"/>
    </edges>
</net>
)V0G0N";

    std::string getModel(gather_tree_test_params p) {
        std::string model = model_t;
        std::string in_out_shape;

        for (auto& dct : p.in_out_shape) {
            in_out_shape += "<dim>";
            in_out_shape += std::to_string(dct) + "</dim>\n";
        }

        REPLACE_WITH_STR(model, "_IN_OUT_", in_out_shape);
        REPLACE_WITH_NUM(model, "_IN2_", p.in_out_shape[1]);

        return model;
    }

protected:
    virtual void TearDown() {
    }

    virtual void SetUp() {
        try {
            TestsCommon::SetUp();
            gather_tree_test_params p = ::testing::WithParamInterface<gather_tree_test_params>::GetParam();
            std::string model = getModel(p);

            Core ie;
            CNNNetwork network = ie.ReadNetwork(model, Blob::CPtr());
            ExecutableNetwork executableNetwork = ie.LoadNetwork(network, p.device_name);
            InferRequest inferRequest = executableNetwork.CreateInferRequest();
            // Output Data
            InferenceEngine::OutputsDataMap out;
            out = network.getOutputsInfo();

            std::pair<std::string, InferenceEngine::DataPtr> item = *out.begin();

            InferenceEngine::TBlob<int32_t>::Ptr output;
            output = InferenceEngine::make_shared_blob<int32_t>(item.second->getTensorDesc());
            output->allocate();

            // Output Reference
            InferenceEngine::TBlob<int32_t> dst_ref(item.second->getTensorDesc());
            dst_ref.allocate();

            // Input Data
            // step_idx
            InferenceEngine::Blob::Ptr step_idx;
            step_idx = InferenceEngine::make_shared_blob<int32_t>({ InferenceEngine::Precision::I32, p.in_out_shape,
                InferenceEngine::TensorDesc::getLayoutByDims(p.in_out_shape) });
            step_idx->allocate();
            memcpy(step_idx->buffer(), &p.step_idx[0], sizeof(int32_t)*p.step_idx.size());
            auto * step_idxPtr = dynamic_cast<InferenceEngine::TBlob<int32_t>*>(step_idx.get());
            if (step_idxPtr == nullptr)
                FAIL() << "Cannot cast blob to TBlob<int32_t>.";

            // parent_idx
            InferenceEngine::Blob::Ptr parent_idx;
            parent_idx = InferenceEngine::make_shared_blob<int32_t>({ InferenceEngine::Precision::I32, p.in_out_shape,
                InferenceEngine::TensorDesc::getLayoutByDims(p.in_out_shape) });
            parent_idx->allocate();
            memcpy(parent_idx->buffer(), &p.parent_idx[0], sizeof(int32_t)*p.parent_idx.size());
            auto * parent_idxPtr = dynamic_cast<InferenceEngine::TBlob<int32_t>*>(parent_idx.get());
            if (parent_idxPtr == nullptr)
                FAIL() << "Cannot cast blob to TBlob<int32_t>.";

            // max_seq_len
            InferenceEngine::Blob::Ptr max_seq_len;
            InferenceEngine::SizeVector max_seq_len_dim(1, p.in_out_shape[1]);
            max_seq_len = InferenceEngine::make_shared_blob<int32_t>({ InferenceEngine::Precision::I32, max_seq_len_dim,
                InferenceEngine::TensorDesc::getLayoutByDims(max_seq_len_dim) });
            max_seq_len->allocate();
            memcpy(max_seq_len->buffer(), &p.max_seq_len[0], sizeof(int32_t)*p.max_seq_len.size());
            auto * max_seq_lenPtr = dynamic_cast<InferenceEngine::TBlob<int32_t>*>(max_seq_len.get());
            if (max_seq_lenPtr == nullptr)
                FAIL() << "Cannot cast blob to TBlob<int32_t>.";

            // end_token
            InferenceEngine::Blob::Ptr end_token;
            InferenceEngine::SizeVector end_token_dim(1, 1);
            end_token = InferenceEngine::make_shared_blob<int32_t>({ InferenceEngine::Precision::I32, end_token_dim,
                InferenceEngine::TensorDesc::getLayoutByDims(end_token_dim) });
            end_token->allocate();
            memcpy(static_cast<int32_t*>(end_token->buffer()), &p.end_token[0], sizeof(int32_t));
            auto * seq_lengthsIdxPtr = dynamic_cast<InferenceEngine::TBlob<int32_t>*>(end_token.get());
            if (seq_lengthsIdxPtr == nullptr)
                FAIL() << "Cannot cast blob to TBlob<int32_t>.";

            // Reference
            ref_gather_tree(*step_idxPtr, *parent_idxPtr, *max_seq_lenPtr, *seq_lengthsIdxPtr, dst_ref);

            if (p.reference.size())
                if (memcmp(dst_ref.data(), &p.reference[0], p.reference.size() * sizeof(int32_t)) != 0)
                    FAIL() << "Wrong result with compare reference vector!";

            // Infer
            inferRequest.SetBlob("step_idx", step_idx);
            inferRequest.SetBlob("parent_idx", parent_idx);
            inferRequest.SetBlob("max_seq_len", max_seq_len);
            inferRequest.SetBlob("end_token", end_token);
            inferRequest.SetBlob(network.getOutputsInfo().begin()->first, output);
            inferRequest.Infer();

            ASSERT_EQ(dst_ref.size(), output->size());
            for (int i = dst_ref.size()-1; i >= 0; i--)
               ASSERT_EQ(dst_ref.data()[i], output->data()[i]);
        } catch (const InferenceEngine::details::InferenceEngineException &e) {
            FAIL() << e.what();
        }
    }
};

TEST_P(GatherTreeTests, TestsGatherTree) {}
