//
//  llm.cpp
//
//  Created by MNN on 2023/08/25.
//  ZhaodeWang
//
// #define MNN_OPEN_TIME_TRACE 1

#include <iostream>
#include <fstream>
#include <regex>

#include <MNN/expr/ExecutorScope.hpp>
#include <MNN/AutoTime.hpp>
#include "llm.hpp"
#include "tokenizer.hpp"

#ifdef USING_VISUAL_MODEL
#include "httplib.h"
#include <cv/cv.hpp>
#endif

// Llm start
Llm* Llm::createLLM(const std::string& path, std::string model_type) {
    auto size = path.size();

    // end with '.mnn' is single model file, otherwise split block models
    bool is_single = (size > 4 &&
                      path[size - 4] == '.' &&
                      path[size - 3] == 'm' &&
                      path[size - 2] == 'n' &&
                      path[size - 1] == 'n');
    Llm* llm = nullptr;
    if (model_type == "auto") {
        model_type = path;
    }
    if (model_type.find("chatglm") != std::string::npos) {
        if (model_type.find("chatglm2") != std::string::npos) {
            llm = new Chatglm2_6b;
        } else if (model_type.find("chatglm3") != std::string::npos) {
            llm = new Chatglm2_6b;
            llm->model_name_ = "Chatglm3_6b";
        } else {
            llm = new Chatglm_6b;
        }
    } else if (model_type.find("codegeex2") != std::string::npos) {
        llm = new Chatglm2_6b;
        llm->model_name_ = "Codegeex2_6b";
    } else if (model_type.find("qwen") != std::string::npos) {
        if (model_type.find("1.8") != std::string::npos) {
            llm = new Qwen_1_8b;
        } else if (model_type.find("vl") != std::string::npos) {
            llm = new Qwen_vl;
        } else {
            llm = new Qwen_7b;
        }
    } else if (model_type.find("llama2") != std::string::npos) {
        llm = new Llama2_7b;
    } else if (model_type.find("baichuan") != std::string::npos) {
        llm = new Llama2_7b;
        llm->model_name_ = "Baichuan2_7b";
    } else if (model_type.find("phi2") != std::string::npos) {
        llm = new Phi_2;
    } else if (model_type.find("internlm") != std::string::npos) {
        llm = new Llama2_7b;
        llm->model_name_ = "Internlm_7b";
    }
    if (!llm) {
        std::cerr << "model type can't judge!" << std::endl;
        return llm;
    }
    llm->is_single_ = is_single;
    std::cout << "### model name : "<< llm->model_name_ << std::endl;
    return llm;
}

void Llm::chat() {
    while (true) {
        std::cout << "\nQ: ";
        std::string input_str;
        std::cin >> input_str;
        if (input_str == "/exit") {
            break;
        }
        if (input_str == "/reset") {
            reset();
            std::cout << "\nA: reset done." << std::endl;
            continue;
        }
        std::cout << "\nA: " << std::flush;
        response(input_str);
        std::cout << std::endl;
    }
    reset();
}

std::string Llm::response(const std::string& query, std::ostream* os, const char* end_with) {
    if (!end_with) {
        end_with = "\n";
    }
    // init status
    gen_seq_len_ = 0;
    all_seq_len_ = 0;
    prefill_us_ = 0;
    decode_us_ = 0;
    past_key_values_.clear();
    if (is_single_) {
        past_key_values_.push_back(_Input(key_value_shape_, NCHW));
    } else {
        for (int i = 0; i < layer_nums_; i++) {
            past_key_values_.push_back(_Input(key_value_shape_, NCHW));
        }
    }
    // response
    auto input_ids = tokenizer(query);
    if (!history_.empty()) {
        std::copy(input_ids.begin(), input_ids.end(), std::back_inserter(history_));
        input_ids = history_;
    } else {
        history_ = input_ids;
    }

    prompt_len_ = static_cast<int>(input_ids.size());
    auto st = std::chrono::system_clock::now();
    int token = forward(input_ids);
    auto et = std::chrono::system_clock::now();
    history_.push_back(token);
    std::string output_str = decode(token);
    prefill_us_ = std::chrono::duration_cast<std::chrono::microseconds>(et - st).count();
    *os << output_str << std::flush;
    while (gen_seq_len_ < max_seq_len_) {
        st = std::chrono::system_clock::now();
        token = forward({token});
        et = std::chrono::system_clock::now();
        decode_us_ += std::chrono::duration_cast<std::chrono::microseconds>(et - st).count();
        if (is_stop(token)) {
            *os << end_with << std::flush;
            break;
        }
        history_.push_back(token);
        auto word = decode(token);
        *os << word << std::flush;
        output_str += word;
    }
#ifdef DUMP_PROFILE_INFO
    print_speed();
#endif
    // update Cache
    // runtime_manager_->updateCache();
    // reset forward info
    return output_str;
}

void Llm::print_speed() {
    auto prefill_s = prefill_us_ * 1e-6;
    auto decode_s = decode_us_ * 1e-6;
    auto total_s = prefill_s + decode_s;
    printf("\n#################################\n");
    printf(" total tokens num  = %d\n", prompt_len_ + gen_seq_len_);
    printf("prompt tokens num  = %d\n", prompt_len_);
    printf("output tokens num  = %d\n", gen_seq_len_);
    printf("  total time = %.2f s\n", total_s);
    printf("prefill time = %.2f s\n", prefill_s);
    printf(" decode time = %.2f s\n", decode_s);
    printf("  total speed = %.2f tok/s\n", (prompt_len_ + gen_seq_len_) / total_s);
    printf("prefill speed = %.2f tok/s\n", prompt_len_ / prefill_s);
    printf(" decode speed = %.2f tok/s\n", gen_seq_len_ / decode_s);
    printf("   chat speed = %.2f tok/s\n", gen_seq_len_ / total_s);
    printf("##################################\n");
}

void Llm::reset() {
    history_.clear();
}

void Llm::load(const std::string& model_dir) {
    model_dir_ = model_dir;
    // init
    ScheduleConfig config;
    BackendConfig cpuBackendConfig;
    config.type          = MNN_FORWARD_CPU;
    // config.type          = MNN_FORWARD_OPENCL;
    config.numThread     = 4;
    cpuBackendConfig.precision = BackendConfig::Precision_Low;
    cpuBackendConfig.memory = BackendConfig::Memory_Low;
    config.backendConfig = &cpuBackendConfig;
    runtime_manager_.reset(Executor::RuntimeManager::createRuntimeManager(config));
    if (config.type == MNN_FORWARD_OPENCL) {
        const char* cacheFileName = ".tempcache";
        // runtime_manager_->setCache(cacheFileName);
    }
    load_progress_ = 0.f;
    printf("load tokenizer\n");
    // 1. load vocab
    std::string tokenizer_path = model_dir + "/tokenizer.txt";
    if (is_single_) {
        size_t pos = model_dir.find_last_of("/\\");
        std::string dir_path = (pos != std::string::npos) ? model_dir.substr(0, pos + 1) : "";
        tokenizer_path = dir_path + "/tokenizer.txt";
    }
    load_progress_ += 5.f;
    tokenizer_->load(tokenizer_path);
    load_progress_ += 5.f;
    printf("load tokenizer Done\n");
    // 2. load model
    Module::Config module_config;
    module_config.shapeMutable = true;
    module_config.rearrange = true;
    if (is_single_) {
        key_value_shape_.insert(key_value_shape_.begin(), layer_nums_);
        modules_.resize(1);
        std::string model_path = model_dir;
        std::string external_path = model_dir + ".weight";
        MNN_PRINT("load %s ... ", model_path.c_str());
        runtime_manager_->setExternalFile(external_path);
        modules_[0].reset(Module::load(
                {"input_ids", "attention_mask", "position_ids", "past_key_values"},
                {"token_id", "presents"}, model_path.c_str(), runtime_manager_, &module_config));
        MNN_PRINT("Done!\n");
        load_progress_ += 90.f;
    } else {
        // 2. load models
        modules_.resize(layer_nums_ + 2);
        float step = 90.0 / modules_.size();
        char buffer[50];
        // load lm model
        std::string lm_model_path = model_dir + "/lm.mnn";
        std::string embedding_model_path = model_dir + "/embedding.mnn";
        MNN_PRINT("[%3.0f%% ] load %s model ... ", load_progress_, lm_model_path.c_str());
        modules_[layer_nums_].reset(Module::load({}, {}, lm_model_path.c_str(), runtime_manager_, &module_config));
        MNN_PRINT("Done!\n");
        load_progress_ += step;
#ifndef USING_DISK_EMBED
        MNN_PRINT("[%3.0f%% ] load %s model ... ", load_progress_, embedding_model_path.c_str());fflush(stdout);
        modules_[layer_nums_ + 1].reset(Module::load({}, {}, embedding_model_path.c_str(), runtime_manager_, &module_config));
        MNN_PRINT("Done!\n");
        load_progress_ += step;
#endif
        if (is_visual_) {
            std::string visual_model_path = model_dir + "/visual.mnn";
            MNN_PRINT("[%3.0f%% ] load %s model ... ", load_progress_, visual_model_path.c_str());fflush(stdout);
            module_config.rearrange = false;
            visual_module_.reset(Module::load({}, {}, visual_model_path.c_str(), runtime_manager_, &module_config));
            MNN_PRINT("Done!\n");
            module_config.rearrange = true;
        }
        // load glm_block models
        for (int i = 0; i < layer_nums_; i++) {
            load_progress_ += step;
            std::string model_path = model_dir + "/block_" + std::to_string(i) + ".mnn";
            MNN_PRINT("[%3.0f%% ] load %s model ... ", load_progress_, model_path.c_str());
            modules_[i].reset(Module::load(
                {"inputs_embeds", "attention_mask", "position_ids", "past_key_values"},
                {"hidden_states", "presents"}, model_path.c_str(), runtime_manager_, &module_config));
            MNN_PRINT("Done!\n");
        }
    }
    if (config.type == MNN_FORWARD_OPENCL) {
        // warmup();
    }
}

void Llm::warmup() {
    // warmup
    MNN_PRINT("### warmup ... ");
    if (is_single_) {
        past_key_values_.push_back(_Input(key_value_shape_, NCHW));
    } else {
        for (int i = 0; i < layer_nums_; i++) {
            past_key_values_.push_back(_Input(key_value_shape_, NCHW));
        }
    }
    std::vector<int> tmp(1, 0);
    forward(tmp);
    all_seq_len_ = 0;
    gen_seq_len_ = 0;
    printf("Done\n");
}

int Llm::forward(const std::vector<int>& input_ids) {
    int seq_len = input_ids.size();
    auto inputs_ids_ = _Const(input_ids.data(), {seq_len}, NCHW, halide_type_of<int>());
    auto attention_mask = gen_attention_mask(seq_len);
    auto position_ids = gen_position_ids(seq_len);
    int id = -1;
    if (is_single_) {
        // single model
        auto outputs = modules_.back()->onForward({inputs_ids_, attention_mask, position_ids, past_key_values_[0]});
        id = outputs[0]->readMap<int>()[0];
        past_key_values_[0] = outputs[1];
    } else {
        // split block models
        auto hidden_states = embedding(input_ids);
        for (int i = 0; i < layer_nums_; i++) {
            AUTOTIME;
            auto outputs = modules_[i]->onForward({hidden_states, attention_mask, position_ids, past_key_values_[i]});
            hidden_states = outputs[0];
            past_key_values_[i] = outputs[1];
        }
        {
            AUTOTIME;
            auto outputs = modules_[layer_nums_]->onForward({hidden_states});
            id = outputs[0]->readMap<int>()[0];
        }

    }
    all_seq_len_ += seq_len;
    gen_seq_len_++;
    return id;
}

VARP Llm::txt_embedding(const std::vector<int>& input_ids) {
#ifndef USING_DISK_EMBED
    // using model forward
    auto inputs_ids_ = _Const(input_ids.data(), {static_cast<int>(input_ids.size())}, NCHW, halide_type_of<int>());
    auto hidden_states = modules_[layer_nums_ + 1]->onForward({inputs_ids_})[0];
    return hidden_states;
#endif
    AUTOTIME;
    // disk embedding to save memory
    size_t seq_len = input_ids.size();
    auto embedding = _Input({static_cast<int>(seq_len), 1, hidden_size_}, NCHW);
    size_t size = hidden_size_ * sizeof(int16_t);
    std::string file_path = model_dir_ + "/embeddings_bf16.bin";
    FILE* file = fopen(file_path.c_str(), "rb");
    std::unique_ptr<int16_t[]> buffer(new int16_t[hidden_size_]);
    for (size_t i = 0; i < seq_len; i++) {
        fseek(file, input_ids[i] * size, SEEK_SET);
        fread(buffer.get(), 1, size, file);
        auto ptr = embedding->writeMap<int16_t>() + i * hidden_size_ * 2;
        for (int j = 0; j < hidden_size_; j++) {
            ptr[j * 2] = 0;
            ptr[j * 2 + 1] = buffer[j];
        }
    }
    fclose(file);
    return embedding;
}

VARP Llm::embedding(const std::vector<int>& input_ids) {
    if (is_visual_ && !gen_seq_len_) {
        return visual_embedding(input_ids);
    }
    return txt_embedding(input_ids);
}

std::vector<int> Llm::tokenizer_encode(const std::string& input_str) {
    auto ids = tokenizer_->encode(input_str);
    return ids;
}

std::string Llm::decode(int id) {
    std::string word = tokenizer_->decode(id);
    // Fix utf-8 garbled characters
    if (word.length() == 6 && word[0] == '<' && word[word.length()-1] == '>' && word[1] == '0' && word[2] == 'x') {
        int num = std::stoi(word.substr(3, 2), nullptr, 16);
        word = static_cast<char>(num);
    }
    return word;
}

// Chatglm_6b
std::vector<int> Chatglm_6b::tokenizer(const std::string& query) {
    auto ids = tokenizer_encode(query);
    context_len_ = ids.size();
    ids.push_back(130001);
    ids.push_back(130004);
    return ids;
}

VARP Chatglm_6b::gen_attention_mask(int seq_len) {
    auto attention_mask = _Input({1, 1, seq_len, seq_len}, NCHW, halide_type_of<int>());
    auto ptr = attention_mask->writeMap<int>();
    for (int i = 0; i < seq_len * seq_len; i++) {
        ptr[i] = 0;
    }
    if (seq_len > 1) {
        for (int i = 1; i < seq_len; i++) {
            ptr[seq_len * i - 1] = 1;
        }
    }
    return attention_mask;
}

VARP Chatglm_6b::gen_position_ids(int seq_len) {
    auto position_ids = _Input({1, 2, seq_len}, NCHW, halide_type_of<int>());
    auto ptr = position_ids->writeMap<int>();
    if (seq_len == 1) {
        ptr[0] = 1;
        ptr[1] = all_seq_len_ - context_len_;
    } else {
        for (int i = 0; i < seq_len; i++) {
            ptr[i] = i;
            ptr[seq_len + i] = 0;
        }
        ptr[2 * seq_len - 1] = 1;
    }
    return position_ids;
}

bool Chatglm_6b::is_stop(int token_id) {
    return token_id == 130005;
}

// Chatglm2_6b
std::vector<int> Chatglm2_6b::tokenizer(const std::string& query) {
    auto prompt = "问：" + query + "\n答：";
    auto ids = tokenizer_encode(prompt);
    if (history_.empty()) {
        ids.insert(ids.begin(), 64792);
        ids.insert(ids.begin(), 64790);
    }
    return ids;
}

VARP Chatglm2_6b::gen_attention_mask(int seq_len) {
    auto attention_mask = _Input({1, 1, seq_len, seq_len}, NCHW, halide_type_of<int>());
    auto ptr = attention_mask->writeMap<int>();
    if (seq_len > 1) {
        for (int i = 0; i < seq_len; i++) {
            for (int j = 0; j < seq_len; j++) {
                ptr[seq_len * i + j] = j > i;
            }
        }
    } else {
        ptr[0] = 0;
    }
    return attention_mask;
}

VARP Chatglm2_6b::gen_position_ids(int seq_len) {
    auto position_ids = _Input({seq_len}, NCHW, halide_type_of<int>());
    auto ptr = position_ids->writeMap<int>();
    if (seq_len == 1) {
        ptr[0] = gen_seq_len_;
    } else {
        for (int i = 0; i < seq_len; i++) {
            ptr[i] = i;
        }
    }
    return position_ids;
}

bool Chatglm2_6b::is_stop(int token_id) {
    return token_id <= 2;
}

// Phi_2
std::vector<int> Phi_2::tokenizer(const std::string& query) {
    auto prompt = query;
    auto ids = tokenizer_encode(prompt);
    return ids;
}

bool Phi_2::is_stop(int token_id) {
    return token_id == 50256;
}

// Qwen_7b
std::vector<int> Qwen_7b::tokenizer(const std::string& query) {
    auto ids = tokenizer_encode(query);
    // auto prompt = "\n<|im_start|>user\n" + query + "<|im_end|>\n<|im_start|>assistant\n";
    ids.insert(ids.begin(), {198, 151644, 872, 198});
    ids.insert(ids.end(), {151645, 198, 151644, 77091, 198});
    return ids;
}

VARP Qwen_7b::gen_attention_mask(int seq_len) {
    auto attention_mask = _Input({1, 1, seq_len, seq_len}, NCHW, halide_type_of<int>());
    auto ptr = attention_mask->writeMap<int>();
    for (int i = 0; i < seq_len; i++) {
        for (int j = 0; j < seq_len; j++) {
            ptr[seq_len * i + j] = j <= i;
        }
    }
    return attention_mask;
}

VARP Qwen_7b::gen_position_ids(int seq_len) {
    auto position_ids = _Input({seq_len}, NCHW, halide_type_of<int>());
    auto ptr = position_ids->writeMap<int>();
    if (seq_len == 1) {
        ptr[0] = all_seq_len_;
    } else {
        for (int i = 0; i < seq_len; i++) {
            ptr[i] = i;
        }
    }
    return position_ids;
}

bool Qwen_7b::is_stop(int token_id) {
    return token_id >= 151645;
}

// Qwen_vl
std::vector<int> Qwen_vl::url_encode(const std::string& url) {
    std::vector<int> ascii_values(imgpad_len_, img_pad_);
    ascii_values[0] = img_start_;
    ascii_values[imgpad_len_ - 1] = img_end_;
    for (int i = 0; i < url.size(); i++) {
        ascii_values[i + 1] = static_cast<int>(url[i]);
    }
    return ascii_values;
}

VARP Qwen_vl::visual_embedding(const std::vector<int>& input_ids) {
#ifdef USING_VISUAL_MODEL
    int start_pos = 0, pad_pos = 0, end_pos = 0;
    for (int i = 0; i < input_ids.size(); i++) {
        int id = input_ids[i];
        if (id == img_start_ && !start_pos) {
            start_pos = i;
        }
        if (id == img_pad_ && !pad_pos) {
            pad_pos = i;
        }
        if (id == img_end_ && !end_pos) {
            end_pos = i;
        }
    }
    if (!start_pos) {
        return txt_embedding(input_ids);
    }
    std::vector<int> prefix(input_ids.begin(), input_ids.begin() + start_pos);
    std::vector<int> img_ascii(input_ids.begin() + start_pos + 1, input_ids.begin() + pad_pos);
    std::vector<int> suffix(input_ids.begin() + end_pos + 1, input_ids.end());
    std::string img_path;
    for (auto ascii_val : img_ascii) {
        img_path += static_cast<char>(ascii_val);
    }
    VARP image = nullptr;
    if (img_path.substr(0, 4) == "http") {
        std::regex url_regex(R"(^https?://([^/]+)(/.*))");
        std::smatch url_match_result;
        std::string host, path;
        if (std::regex_search(img_path, url_match_result, url_regex) && url_match_result.size() == 3) {
            host = url_match_result[1].str();
            path = url_match_result[2].str();
        }
        std::cout << host << "#" << path << std::endl;
        httplib::Client cli(host);
        auto res = cli.Get(path);
        std::string img_file = "downloaded_image.jpg";
        if (res && res->status == 200) {
            std::ofstream file(img_file, std::ios::binary);
            if (file.is_open()) {
                file.write(res->body.c_str(), res->body.size());
                std::cout << "Image has been downloaded successfully." << std::endl;
                file.close();
            } else {
                std::cerr << "Unable to open file to write image." << std::endl;
                exit(0);
            }
        } else {
            std::cerr << "Failed to download image. Status code: " << (res ? res->status : 0) << std::endl;
            exit(0);
        }
        image = MNN::CV::imread(img_file);
    } else {
        image = MNN::CV::imread(img_path);
    }
    image = MNN::CV::resize(image, {img_size_, img_size_}, 0, 0, MNN::CV::INTER_LINEAR, MNN::CV::COLOR_BGR2RGB,
                            {123.25239296, 117.20384, 104.50194688}, {0.0145414 , 0.01494914, 0.01416452});
    image = MNN::Express::_Unsqueeze(image, {0});
    image = MNN::Express::_Convert(image, NC4HW4);
    auto image_embedding = visual_module_->forward(image);
    image_embedding = MNN::Express::_Permute(image_embedding, {1, 0, 2});
    auto prefix_embedding = txt_embedding(prefix);
    auto suffix_embedding = txt_embedding(suffix);
    auto embeddings = MNN::Express::_Concat({prefix_embedding, image_embedding, suffix_embedding}, 0);
#else
    auto embeddings = txt_embedding(input_ids);
#endif
    return embeddings;
}

std::vector<int> Qwen_vl::tokenizer(const std::string& query) {
    // split query
    std::regex img_regex("<img>(.*?)</img>");
    std::string::const_iterator searchStart(query.cbegin());
    std::smatch match;
    std::vector<std::string> img_info, txt_info;
    std::vector<int> ids {};
    while (std::regex_search(searchStart, query.cend(), match, img_regex)) {
        auto txt_ids = tokenizer_encode(match.prefix().str());
        ids.insert(ids.end(), txt_ids.begin(), txt_ids.end());
        auto img_ids = url_encode(match[1].str());
        ids.insert(ids.end(), img_ids.begin(), img_ids.end());
        searchStart = match.suffix().first;
    }
    if (searchStart != query.cend()) {
        auto txt_ids = tokenizer_encode(std::string(searchStart, query.cend()));
        ids.insert(ids.end(), txt_ids.begin(), txt_ids.end());
    }
    // auto prompt = "\n<|im_start|>user\n" + query + "<|im_end|>\n<|im_start|>assistant\n";
    ids.insert(ids.begin(), {198, 151644, 872, 198});
    ids.insert(ids.end(), {151645, 198, 151644, 77091, 198});
    return ids;
}

VARP Qwen_vl::gen_attention_mask(int seq_len) {
    if (seq_len == 1) {
        auto attention_mask = _Input({1, 1, 1, all_seq_len_ + 1}, NCHW, halide_type_of<float>());
        auto ptr = attention_mask->writeMap<float>();
        for (int i = 0; i < all_seq_len_ + 1; i++) {
            ptr[i] = 0;
        }
        return attention_mask;
    } else {
        auto attention_mask = _Input({1, 1, seq_len, seq_len}, NCHW, halide_type_of<float>());
        auto ptr = attention_mask->writeMap<float>();
        for (int i = 0; i < seq_len; i++) {
            for (int j = 0; j < seq_len; j++) {
                ptr[seq_len * i + j] = (j > i) * std::numeric_limits<float>::lowest();
            }
        }
        return attention_mask;
    }
}

// Llama2_7b
std::vector<int> Llama2_7b::tokenizer(const std::string& query) {
    auto ids = tokenizer_encode(query);
    if (model_name_ == "Baichuan2_7b") {
        // baichuan2: <reserved_106>{query}<reserved_107>: 195, query, 196
        ids.insert(ids.begin(), 195);
        ids.push_back(196);
        return ids;
    }
    if (model_name_ == "Internlm_7b") {
        // internlm: "<|User|>:" + query + "<eoh>\n<|Bot|>:";
        // 1, 333, 352, 1621, 352, 27232, query, 103027, 364, 333, 352, 23845, 352, 27232
        ids.insert(ids.begin(), {1, 333, 352, 1621, 352, 27232});
        ids.insert(ids.end(), {103027, 364, 333, 352, 23845, 352, 27232});
        return ids;
    }
    // llama2: <bos>[INST]{query}[/INST]: 1, 5539, 25580, 29962, query, 12452, 25580, 29962
    ids.insert(ids.begin(), {1, 5539, 25580, 29962});
    ids.insert(ids.end(), {12452, 25580, 29962});
    return ids;
}

VARP Llama2_7b::gen_attention_mask(int seq_len) {
    if (seq_len == 1) {
        auto attention_mask = _Input({1, 1, 1, all_seq_len_ + 1}, NCHW, halide_type_of<float>());
        auto ptr = attention_mask->writeMap<float>();
        for (int i = 0; i < all_seq_len_ + 1; i++) {
            ptr[i] = 0;
        }
        return attention_mask;
    } else {
        auto attention_mask = _Input({1, 1, seq_len, seq_len}, NCHW, halide_type_of<float>());
        auto ptr = attention_mask->writeMap<float>();
        for (int i = 0; i < seq_len; i++) {
            for (int j = 0; j < seq_len; j++) {
                ptr[seq_len * i + j] = (j > i) * std::numeric_limits<float>::lowest();
            }
        }
        return attention_mask;
    }
}

VARP Llama2_7b::gen_position_ids(int seq_len) {
    auto position_ids = _Input({1, seq_len}, NCHW, halide_type_of<int>());
    auto ptr = position_ids->writeMap<int>();
    if (seq_len == 1) {
        ptr[0] = all_seq_len_;
    } else {
        for (int i = 0; i < seq_len; i++) {
            ptr[i] = i;
        }
    }
    return position_ids;
}

bool Llama2_7b::is_stop(int token_id) {
    if (model_name_ == "Internlm_7b") {
        // 103028: <eoa>
        return token_id == 2 || token_id == 103028;
    }
    return token_id == 2;
}
// Llm end

// Embedding start
float Embedding::dist(VARP var0, VARP var1) {
    auto distVar = _Sqrt(_ReduceSum(_Square(var0 - var1)));
    auto dist = distVar->readMap<float>()[0];
    return dist;
}

Embedding* Embedding::createEmbedding(const std::string& path, std::string model_type) {
    auto size = path.size();

    Embedding* embedding = nullptr;
    if (model_type == "auto") {
        model_type = path;
    }
    if (model_type.find("bge") != std::string::npos) {
        embedding = new Bge;
    }
    if (!embedding) {
        std::cerr << "model type can't judge!" << std::endl;
        return embedding;
    }
    std::cout << "### model name : "<< embedding->model_name_ << std::endl;
    return embedding;
}

void Embedding::load(const std::string& model_dir) {
    model_dir_ = model_dir;
    // init
    ScheduleConfig config;
    BackendConfig cpuBackendConfig;
    config.type          = MNN_FORWARD_CPU;
    // config.type          = MNN_FORWARD_OPENCL;
    config.numThread     = 4;
    cpuBackendConfig.precision = BackendConfig::Precision_Low;
    cpuBackendConfig.memory = BackendConfig::Memory_Low;
    config.backendConfig = &cpuBackendConfig;
    runtime_manager_.reset(Executor::RuntimeManager::createRuntimeManager(config));
    printf("load tokenizer\n");
    // 1. load vocab
    size_t pos = model_dir.find_last_of("/\\");
    std::string dir_path = (pos != std::string::npos) ? model_dir.substr(0, pos + 1) : "";
    std::string tokenizer_path = dir_path + "/tokenizer.txt";
    tokenizer_->load(tokenizer_path);
    printf("load tokenizer Done\n");
    // 2. load model
    Module::Config module_config;
    module_config.shapeMutable = true;
    module_config.rearrange = true;
    std::string model_path = model_dir;
    MNN_PRINT("load %s ... ", model_path.c_str());
    module_.reset(Module::load(
            {"input_ids", "attention_mask", "position_ids"},
            {"sentence_embeddings"}, model_path.c_str(), runtime_manager_, &module_config));
    MNN_PRINT("Done!\n");
}

VARP Embedding::embedding(const std::string& txt) {
    auto ids = tokenizer(txt);
    prompt_len_ = ids.size();
    auto inputs_ids = _Const(ids.data(), {prompt_len_}, NCHW, halide_type_of<int>());
    auto attention_mask = gen_attention_mask(prompt_len_);
    auto position_ids = gen_position_ids(prompt_len_);
    auto outputs = module_->onForward({inputs_ids, attention_mask, position_ids});
    auto sentence_embeddings = outputs[0];
    return sentence_embeddings;
}

void Embedding::print_speed() {
    auto total_s = embedding_us_ * 1e-6;
    printf("\n#################################\n");
    printf("  total token = %d\n", prompt_len_);
    printf("  total time  = %.2f s\n", total_s);
    printf("  total speed = %.2f tok/s\n", prompt_len_ / total_s);
    printf("##################################\n");
}

std::vector<int> Embedding::tokenizer_encode(const std::string& input_str) {
    auto ids = tokenizer_->encode(input_str);
    return ids;
}

std::vector<int> Bge::tokenizer(const std::string& query) {
    auto ids = tokenizer_encode(query);
    ids.insert(ids.begin(), 101);
    ids.push_back(102);
    return ids;
}

VARP Bge::gen_attention_mask(int seq_len) {
    auto attention_mask = _Input({1, 1, 1, seq_len}, NCHW, halide_type_of<int>());
    auto ptr = attention_mask->writeMap<int>();
    for (int i = 0; i < seq_len; i++) {
        ptr[i] = 1;
    }
    return attention_mask;
}

VARP Bge::gen_position_ids(int seq_len) {
    auto position_ids = _Input({1, seq_len}, NCHW, halide_type_of<int>());
    auto ptr = position_ids->writeMap<int>();
    for (int i = 0; i < seq_len; i++) {
        ptr[i] = i;
    }
    return position_ids;
}
// Embedding end

// TextVectorStore strat
TextVectorStore* TextVectorStore::load(const std::string& path) {
    auto vars = Variable::load(path.c_str());
    if (vars.size() < 2) {
        return nullptr;
    }
    TextVectorStore* store = new TextVectorStore;
    store->vectors_ = vars[0];
    for (int i = 1; i < vars.size(); i++) {
        const char* txt = vars[i]->readMap<char>();
        store->texts_.push_back(txt);
    }
    return store;
}

void TextVectorStore::save(const std::string& path) {
    std::vector<VARP> vars;
    vars.push_back(vectors_);
    for (auto text : texts_) {
        auto text_var = _Const(text.data(), {text.size()}, NHWC, halide_type_of<int8_t>());
        vars.push_back(text_var);
    }
    Variable::save(vars, path.c_str());
}

void TextVectorStore::add_text(const std::string& text) {
    auto vector = text2vector(text);
    texts_.push_back(text);
    if (vectors_ == nullptr) {
        vectors_ = vector;
    } else {
        vectors_ = _Concat({vectors_, vector}, 0);
    }
    vectors_.fix(VARP::CONSTANT);
}

void TextVectorStore::add_texts(const std::vector<std::string>& texts) {
    for (const auto& text : texts) {
        add_text(text);
    }
}

std::vector<std::string> TextVectorStore::search_similar_texts(const std::string& text, int topk) {
    auto vector = text2vector(text);
    auto dist = _Sqrt(_ReduceSum(_Square(vectors_ - vector), {-1}));
    auto indices = _Sort(dist, 0, true);
    // auto ptr = dist->readMap<float>();
    auto idx_ptr = indices->readMap<int>();
    std::vector<std::string> res;
    for (int i = 0; i < topk; i++) {
        int pos = idx_ptr[i];
        if (pos >= 0 && pos < texts_.size()) {
            res.push_back(texts_[pos]);
        }
    }
    return res;
}

void TextVectorStore::bench() {
    const int n = 50000;
    const int d = 1024;
    std::vector<int> shape0_v = {n, d};
    std::vector<int> shape1_v = {1, d};
    auto shape0 = _Const(shape0_v.data(), {2});
    auto shape1 = _Const(shape1_v.data(), {2});
    vectors_ = _RandomUnifom(shape0, halide_type_of<float>());
    auto vec = _RandomUnifom(shape1, halide_type_of<float>());
    auto start = std::chrono::high_resolution_clock::now();
    auto dist = _Sqrt(_ReduceSum(_Square(vectors_ - vec), {-1}));
    auto indices = _Sort(dist, 0, true);
    auto ptr = dist->readMap<float>();
    auto iptr = indices->readMap<int>();
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "search took " << duration.count() << " milliseconds." << std::endl;
    for (int i = 0; i < 5; i++) {
        printf("index: %d, distance: %f\n", iptr[i], ptr[iptr[i]]);
    }
    vectors_ = nullptr;
}

VARP TextVectorStore::text2vector(const std::string& text) {
    auto vector = embedding_->embedding(text);
    return vector;
}

// TextVectorStore end