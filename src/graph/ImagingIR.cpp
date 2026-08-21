#include "latent/graph/ImagingIR.h"

#include <sstream>

namespace latent::graph {

ValueId ImagingGraph::addInput(std::string name, imaging::ImageType type) {
    const auto id = static_cast<ValueId>(values_.size());
    values_.push_back(Value{id, std::move(name), type, true});
    return id;
}

ValueId ImagingGraph::addOperation(OperationDescriptor descriptor) {
    const auto id = static_cast<ValueId>(values_.size());
    values_.push_back(Value{id, descriptor.name, descriptor.outputType, false});
    operations_.push_back(Operation{std::move(descriptor), id});
    return id;
}

GraphValidation ImagingGraph::validate() const {
    GraphValidation result{};

    for (const auto& value : values_) {
        const auto typeValidation = imaging::validateImageType(value.type);
        if (!typeValidation.valid) {
            result.valid = false;
            result.errors.push_back("value '" + value.name + "': " + typeValidation.message);
        }
    }

    for (std::size_t opIndex = 0; opIndex < operations_.size(); ++opIndex) {
        const auto& op = operations_[opIndex];
        if (op.descriptor.inputs.empty()) {
            result.valid = false;
            result.errors.push_back("operation '" + op.descriptor.name + "' has no input");
        }

        for (const auto input : op.descriptor.inputs) {
            if (input >= op.output || input >= values_.size()) {
                result.valid = false;
                std::ostringstream message;
                message << "operation '" << op.descriptor.name
                        << "' references unavailable value " << input;
                result.errors.push_back(message.str());
            }
        }

        if (op.descriptor.temporal && op.descriptor.access != AccessPattern::Temporal) {
            result.valid = false;
            result.errors.push_back(
                "operation '" + op.descriptor.name + "' is temporal but has non-temporal access pattern");
        }

        if (!op.descriptor.inputs.empty()) {
            const auto& inputType = values_.at(op.descriptor.inputs.front()).type;
            const bool domainActuallyChanges = inputType.reference != op.descriptor.outputType.reference;
            if (op.descriptor.changesReferenceDomain && !domainActuallyChanges) {
                result.valid = false;
                result.errors.push_back(
                    "operation '" + op.descriptor.name + "' declares a reference-domain change without changing domain");
            }
            if (!op.descriptor.changesReferenceDomain && domainActuallyChanges) {
                result.valid = false;
                result.errors.push_back(
                    "operation '" + op.descriptor.name + "' changes reference domain without declaring it");
            }
        }
    }

    return result;
}

std::vector<FusionGroup> ImagingGraph::planConservativeFusion() const {
    std::vector<FusionGroup> groups;
    if (operations_.empty()) {
        return groups;
    }

    std::vector<std::size_t> consumerCount(values_.size(), 0);
    for (const auto& op : operations_) {
        for (const auto input : op.descriptor.inputs) {
            if (input < consumerCount.size()) {
                ++consumerCount[input];
            }
        }
    }

    auto isFusionSafe = [](const OperationDescriptor& descriptor) {
        return descriptor.pure && descriptor.canFuse && !descriptor.temporal &&
               descriptor.access != AccessPattern::Reduction &&
               descriptor.access != AccessPattern::Temporal &&
               descriptor.access != AccessPattern::External;
    };

    FusionGroup current{};
    for (std::size_t index = 0; index < operations_.size(); ++index) {
        const auto& op = operations_[index];
        bool canAppend = isFusionSafe(op.descriptor);

        if (!current.operationIndices.empty() && canAppend) {
            const auto previousIndex = current.operationIndices.back();
            const auto& previous = operations_[previousIndex];
            const bool directlyConsumesPrevious =
                op.descriptor.inputs.size() == 1 && op.descriptor.inputs.front() == previous.output;
            const bool previousHasSingleConsumer = consumerCount[previous.output] == 1;
            canAppend = directlyConsumesPrevious && previousHasSingleConsumer &&
                        isFusionSafe(previous.descriptor);
        }

        if (!canAppend || current.operationIndices.empty()) {
            if (!current.operationIndices.empty()) {
                groups.push_back(current);
                current.operationIndices.clear();
            }
            current.operationIndices.push_back(index);
        } else {
            current.operationIndices.push_back(index);
        }
    }

    if (!current.operationIndices.empty()) {
        groups.push_back(std::move(current));
    }

    return groups;
}

}  // namespace latent::graph
