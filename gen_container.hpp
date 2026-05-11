/**
 * @file GenContainerWithTransform.hpp
 * @brief Generalized multi-index container with integrated data transformation.
 *
 * This container supports:
 * - Multi-key hash indexing
 * - Range queries
 * - Partial/template matching via visitor pattern
 * - Bulk insertion with parallel index updates
 * - Parallel sorting
 * - Bit-packed numeric field storage
 * - Direct template-based transformation of records with token replacement and callbacks
 *
 * @author Anthony Matarazzo
 * @copyright (c) 2026 Anthony Matarazzo
 * @license GPL-3.0-or-later
 */

#pragma once
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <execution>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <optional>
#include <functional>
#include <future>
#include <regex>
#include <type_traits>

/**
 * @brief Bit-packing helper for integral fields.
 * @tparam T Integral type
 * @tparam Bits Number of bits to store
 */
template<typename T, size_t Bits>
struct BitPacker {
    static_assert(std::is_integral<T>::value, "BitPacker requires integral types.");
    using storage_type = uint64_t;

    static storage_type pack(T value, T minValue, T maxValue) {
        double normalized = double(value - minValue) / (maxValue - minValue);
        return static_cast<storage_type>(normalized * ((1ULL << Bits) - 1));
    }

    static T unpack(storage_type packed, T minValue, T maxValue) {
        double normalized = double(packed) / ((1ULL << Bits) - 1);
        return minValue + static_cast<T>(normalized * (maxValue - minValue));
    }
};

/**
 * @brief Visitor interface for filtering records.
 * @tparam T Record type
 */
template<typename T>
struct Visitor {
    virtual bool operator()(const T& item) const = 0;
    virtual ~Visitor() = default;
};

/**
 * @brief Visitor interface for sorting records.
 * @tparam T Record type
 */
template<typename T>
struct SortVisitor {
    virtual bool operator()(const T& a, const T& b) const = 0;
    virtual ~SortVisitor() = default;
};

/**
 * @brief Embedded data transformer for token-based string transformation.
 * @tparam T Record type
 */
template<typename T>
struct DataTransformer {
    using FormatterFunc = std::function<std::string(const T&)>;

    std::unordered_map<std::string, FormatterFunc> fieldFormatters;

    void registerFormatter(const std::string& token, FormatterFunc func) {
        fieldFormatters[token] = func;
    }

    std::string transformRecord(const T& record, const std::string& templateStr) const {
        std::string output = templateStr;
        std::regex tokenRegex(R"(\{([a-zA-Z0-9_]+)\})");
        std::smatch match;
        std::string::const_iterator searchStart(output.cbegin());

        while (std::regex_search(searchStart, output.cend(), match, tokenRegex)) {
            std::string token = match[1].str();
            std::string replacement;

            auto it = fieldFormatters.find(token);
            if (it != fieldFormatters.end()) replacement = it->second(record);
            else replacement = "{" + token + "}";

            output.replace(match.position(), match.length(), replacement);
            searchStart = output.cbegin() + match.position() + replacement.size();
        }
        return output;
    }

    std::vector<std::string> transformRecords(const std::vector<T>& records, const std::string& templateStr) const {
        std::vector<std::string> results(records.size());
        std::vector<std::future<void>> futures;
        for (size_t i = 0; i < records.size(); ++i) {
            futures.push_back(std::async(std::launch::async, [&](size_t idx){
                results[idx] = transformRecord(records[idx], templateStr);
            }, i));
        }
        for (auto& f : futures) f.get();
        return results;
    }
};

/**
 * @brief Generalized multi-index container with integrated transformation.
 * @tparam T Record type
 * @tparam Keys Types of key fields for multi-index hashing
 */
template<typename T, typename... Keys>
class GenContainer {
private:
    std::vector<T> elements;
    std::tuple<std::unordered_map<Keys, size_t>...> indices;
    mutable std::shared_mutex containerMutex;

    template<size_t I = 0>
    void updateIndex(const T& element, size_t pos) {
        if constexpr(I < sizeof...(Keys)) {
            auto& idx = std::get<I>(indices);
            idx[getKey<I>(element)] = pos;
            updateIndex<I+1>(element, pos);
        }
    }

    template<size_t I = 0>
    auto getKey(const T& element) const {
        if constexpr(I < sizeof...(Keys)) return element.template key<I>();
        else static_assert(I < sizeof...(Keys), "Key index out of range");
    }

public:
    GenContainer() = default;

    /** @brief Add a single record */
    void add(const T& item) {
        std::unique_lock lock(containerMutex);
        elements.push_back(item);
        updateIndex(item, elements.size() - 1);
    }

    /** @brief Bulk insertion with parallel index updates */
    void addBulk(const std::vector<T>& items) {
        std::unique_lock lock(containerMutex);
        size_t start = elements.size();
        elements.insert(elements.end(), items.begin(), items.end());

        auto updateFunc = [&](size_t i){ updateIndex(elements[i], i); };
        std::vector<std::future<void>> futures;
        for (size_t i = start; i < elements.size(); ++i)
            futures.push_back(std::async(std::launch::async, updateFunc, i));
        for (auto& f : futures) f.get();
    }

    /** @brief Parallel sort using visitor */
    void parallelSort(const SortVisitor<T>& visitor) {
        std::unique_lock lock(containerMutex);
        std::sort(std::execution::par, elements.begin(), elements.end(),
                  [&](const T& a, const T& b){ return visitor(a, b); });
        for (size_t i = 0; i < elements.size(); ++i) updateIndex(elements[i], i);
    }

    /** @brief Hash search by key index */
    template<size_t KeyIndex>
    std::optional<T> hashSearch(const typename std::tuple_element<KeyIndex,std::tuple<Keys...>>::type& key) const {
        std::shared_lock lock(containerMutex);
        auto& idx = std::get<KeyIndex>(indices);
        auto it = idx.find(key);
        if(it != idx.end()) return elements[it->second];
        return {};
    }

    /** @brief Range query on numeric field */
    template<typename FieldType>
    std::vector<T> rangeQuery(FieldType T::*field, FieldType minVal, FieldType maxVal) const {
        std::vector<T> result;
        std::shared_lock lock(containerMutex);
        for(const auto& item : elements)
            if(auto val = item.*field; val >= minVal && val <= maxVal) result.push_back(item);
        return result;
    }

    /** @brief Partial/template match using visitor */
    std::vector<T> match(const Visitor<T>& visitor) const {
        std::vector<T> result;
        std::shared_lock lock(containerMutex);
        for(const auto& item : elements)
            if(visitor(item)) result.push_back(item);
        return result;
    }

    /**
     * @brief Transform records directly from container
     * @param templateStr Template string with {tokens}
     * @param transformer DataTransformer with callbacks
     * @param subset Optional subset of records (defaults to full container)
     */
    std::vector<std::string> transform(
        const std::string& templateStr,
        const DataTransformer<T>& transformer,
        const std::optional<std::vector<T>>& subset = std::nullopt
    ) const {
        std::vector<T> dataToTransform = subset.value_or(elements);
        return transformer.transformRecords(dataToTransform, templateStr);
    }

    /** @brief Iterators */
    auto begin() { return elements.begin(); }
    auto end() { return elements.end(); }
    size_t size() const { return elements.size(); }
};
