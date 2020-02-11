#include "path_template.h"

#include <util/generic/vector.h>

#include <numeric>

namespace NYtPathTemplate {
    NYT::TCypressPath TPathTemplate::ToPathImpl(const std::initializer_list<NYT::TCypressPath>& emptySubPaths) const {
        auto emptyIt = std::rbegin(emptySubPaths);
        TVector<NYT::TCypressPath> tail;
        for (auto it = this; it; it = it->Parent_.Get()) {
            if (!it->Path_.has_value()) {
                Y_VERIFY(emptyIt != std::rend(emptySubPaths));
                tail.emplace_back(*emptyIt++);
            } else {
                tail.emplace_back(it->Path_.value());
            }
        }

        Y_VERIFY(emptyIt == std::rend(emptySubPaths) && !tail.empty());
        return std::accumulate(
            std::next(tail.rbegin()),
            tail.rend(),
            tail.back(),
            [](const NYT::TCypressPath& l, const NYT::TCypressPath& r) {
                return l / r;
            });
    }

    TPathTemplatePtr MakeRootNode(std::optional<NYT::TCypressPath> path) {
        return TPathTemplatePtr(new TPathTemplate(std::move(path), TPathTemplatePtr()));
    }

    TPathTemplatePtr MakeChildNode(
        TPathTemplatePtr parent,
        std::optional<NYT::TCypressPath> path)
    {
        Y_VERIFY(parent.Get());
        return TPathTemplatePtr(new TPathTemplate(std::move(path), std::move(parent)));
    }

}
