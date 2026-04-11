#include <fstream>
#include <iostream>
#include <vector>
#include "tflite-micro/tensorflow/lite/schema/schema_generated.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <model.tflite>\n";
        return 1;
    }
    std::ifstream ifs(argv[1], std::ios::binary | std::ios::ate);
    if (!ifs) {
        std::cerr << "Cannot open " << argv[1] << "\n";
        return 1;
    }
    auto size = ifs.tellg();
    ifs.seekg(0);
    std::vector<char> data(size);
    ifs.read(data.data(), size);
    const tflite::Model* model = tflite::GetModel(data.data());
    if (!model) {
        std::cerr << "GetModel failed\n";
        return 2;
    }
    auto subgraphs = model->subgraphs();
    std::cout << "subgraphs=" << (subgraphs ? subgraphs->size() : 0) << "\n";
    if (!subgraphs || subgraphs->size() == 0) return 3;
    auto sub = subgraphs->Get(0);
    auto inputs = sub->inputs();
    auto outputs = sub->outputs();
    auto tensors = sub->tensors();
    std::cout << "tensors=" << (tensors ? tensors->size() : 0) << "\n";
    if(inputs) {
      std::cout << "inputs size=" << inputs->size() << "\n";
      for (auto i : *inputs) {
        auto t = tensors->Get(i);
        std::cout << "input " << i << ": name=" << (t->name() ? t->name()->c_str() : "<noname>") << " type=" << t->type() << "\n";
        auto shape = t->shape();
        std::cout << "  shape:";
        if(shape) for(auto d:*shape) std::cout << " " << d;
        std::cout << "\n";
      }
    }
    if(outputs) {
      std::cout << "outputs size=" << outputs->size() << "\n";
      for (auto i : *outputs) {
        auto t = tensors->Get(i);
        std::cout << "output " << i << ": name=" << (t->name() ? t->name()->c_str() : "<noname>") << " type=" << t->type() << "\n";
        auto shape = t->shape();
        std::cout << "  shape:";
        if(shape) for(auto d:*shape) std::cout << " " << d;
        std::cout << "\n";
      }
    }
    const auto* opcodes = model->operator_codes();
    std::cout << "opcodes=" << (opcodes ? opcodes->size() : 0) << "\n";
    if(opcodes) {
      for(size_t i=0;i<opcodes->size();i++) {
        auto op = opcodes->Get(i);
        std::cout << "opcode["<<i<<"] builtin_code="<<op->builtin_code();
        if(op->custom_code()) std::cout<<" custom="<<op->custom_code()->c_str();
        std::cout<<"\n";
      }
    }
    return 0;
}
