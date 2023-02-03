/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef MLIR_HLO_TOOLS_MLIR_INTERPRETER_FRAMEWORK_TENSOR_OR_MEMREF_H_
#define MLIR_HLO_TOOLS_MLIR_INTERPRETER_FRAMEWORK_TENSOR_OR_MEMREF_H_

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <math.h>

#include "llvm/ADT/ArrayRef.h"
#include "mlir/Support/LLVM.h"

namespace mlir {
namespace interpreter {

// Represents a view into a physical buffer.
struct BufferView {
  int64_t offset;
  llvm::SmallVector<int64_t> sizes;    // [10, 11, 12]
  llvm::SmallVector<int64_t> strides;  // [132, 12, 1]
  // Number of vector element dimensions in the tensor. nullopt if this is a
  // vector itself (isVector is set). {0} if this is a tensor of a unit vector.
  std::optional<int64_t> numVectorDims = std::nullopt;
  bool isVector = false;

  int64_t rank() const { return sizes.size() - numVectorDims.value_or(0); }

  // Removes the dimension from the view. If you need to keep it, use the
  // overload below with dimSize = 1.
  void slice(int64_t dimIndex, int64_t dimOffset);
  void slice(int64_t dimIndex, int64_t dimOffset, int64_t dimSize,
             int64_t dimStride = 1);
  bool subview(ArrayRef<int64_t> subviewoffsets, ArrayRef<int64_t> subviewsizes,
               ArrayRef<int64_t> subviewstrides);
  int64_t getNumElements(bool includeVectorDims = false) const;

  class LogicalIndexView {
   public:
    class Iterator {
     public:
      using iterator_category = std::forward_iterator_tag;
      using value_type = llvm::SmallVector<int64_t>;
      using difference_type = std::ptrdiff_t;
      using pointer = llvm::SmallVector<int64_t>*;
      using reference = llvm::SmallVector<int64_t>&;

      const llvm::SmallVector<int64_t>& operator*() const {
        return viewIndices;
      }
      const llvm::SmallVector<int64_t>* operator->() const {
        return &viewIndices;
      }

      Iterator& operator++() {
        auto indexIt = viewIndices.rbegin();
        auto sizeIt = view->sizes.rbegin();
        if (!includeVectorDims) {
          std::advance(sizeIt, view->numVectorDims.value_or(0));
        }

        for (auto e = viewIndices.rend(); indexIt != e; ++indexIt, ++sizeIt) {
          ++*indexIt;
          if (*indexIt < *sizeIt) {
            return *this;
          }
          *indexIt = 0;
        }

        viewIndices.clear();
        viewIndices.push_back(-1);
        return *this;
      }

      Iterator operator++(int) {
        auto tmp = *this;
        ++(*this);
        return tmp;
      }

      bool operator==(const Iterator& other) const {
        return viewIndices == other.viewIndices;
      }

      bool operator!=(const Iterator& other) const { return !(*this == other); }

     private:
      friend class LogicalIndexView;

      Iterator(const BufferView* view, llvm::SmallVector<int64_t> indices,
               bool includeVectorDims)
          : view(view),
            viewIndices(std::move(indices)),
            includeVectorDims(includeVectorDims) {}

      const BufferView* view;
      llvm::SmallVector<int64_t> viewIndices;
      bool includeVectorDims;
    };

    Iterator begin() const {
      if (view->getNumElements() == 0) return end();
      return {view,
              llvm::SmallVector<int64_t>(
                  view->rank() +
                  (includeVectorDims ? view->numVectorDims.value_or(0) : 0)),
              includeVectorDims};
    }
    Iterator end() const { return {view, {-1}, false}; }

   private:
    friend class BufferView;

    LogicalIndexView(const BufferView* view, bool includeVectorDims)
        : view(view), includeVectorDims(includeVectorDims) {}

    const BufferView* view;
    bool includeVectorDims;
  };

  int64_t getPhysicalIndex(llvm::ArrayRef<int64_t> viewindices) const;
  LogicalIndexView indices(bool includeVectorDims = false) const {
    return LogicalIndexView{this, includeVectorDims};
  }
  // Returns the stride resulting from collapsing the given dimensions, if
  // possible.
  std::optional<int64_t> getCollapsedStride(llvm::ArrayRef<int64_t> dims) const;

  bool inBounds(llvm::ArrayRef<int64_t> viewIndices) const;
  static SmallVector<int64_t> getDefaultStrides(ArrayRef<int64_t> sizes);
  static SmallVector<int64_t> getStridesForLayout(ArrayRef<int64_t> sizes,
                                                  ArrayRef<int64_t> layout);
};

// Backing for a TensorOrMemref.
class Buffer {
 private:
  struct Dummy {};

 public:
  template <typename T>
  static std::shared_ptr<Buffer> allocate(size_t size) {
    return std::make_shared<Buffer>(Dummy{}, size, sizeof(T));
  }

  char* at(int64_t idx, int64_t elementSize) {
    assert(!isDeallocated && "accessing deallocated buffer");
    return &storage.data()[idx * elementSize];
  }

  const char* at(int64_t idx, int64_t elementSize) const {
    assert(!isDeallocated && "accessing deallocated buffer");
    return &storage.data()[idx * elementSize];
  }

  Buffer(Dummy, size_t numElements, size_t elementSize)
      : storage(numElements * elementSize) {}

  int64_t getByteSize() const { return storage.size(); }

  void deallocate() { isDeallocated = true; }

  bool deallocated() const { return isDeallocated; }

 private:
  llvm::SmallVector<char> storage;
  bool isDeallocated = false;
};

template <typename T>
struct TensorOrMemref {
  using element_type = T;

  static TensorOrMemref<T> empty(ArrayRef<int64_t> sizes,
                                 ArrayRef<int64_t> layout = {}) {
    BufferView dummy{0, SmallVector<int64_t>(sizes), {}};
    return emptyLike(dummy, layout);
  }

  static TensorOrMemref<T> emptyLike(const BufferView& view,
                                     ArrayRef<int64_t> layout = {}) {
    BufferView newView = view;
    newView.offset = 0;
    newView.strides = BufferView::getStridesForLayout(view.sizes, layout);
    return {Buffer::allocate<T>(view.getNumElements(true)), newView};
  }

  TensorOrMemref<T> clone(ArrayRef<int64_t> layout = {}) const {
    auto out = emptyLike(view, layout);
    for (auto [src_index, dst_index] :
         llvm::zip(view.indices(true), out.view.indices(true))) {
      out.at(dst_index) = at(src_index);
    }
    return out;
  }

  const T& at(ArrayRef<int64_t> indices) const {
    assert(view.inBounds(indices) && "out of bounds");
    return *reinterpret_cast<const T*>(
        buffer->at(view.getPhysicalIndex(indices), sizeof(T)));
  }

  T& at(ArrayRef<int64_t> indices) {
    assert(view.inBounds(indices) && "out of bounds");
    return *reinterpret_cast<T*>(
        buffer->at(view.getPhysicalIndex(indices), sizeof(T)));
  }

  TensorOrMemref vectorAt(ArrayRef<int64_t> indices) const {
    assert(view.inBounds(indices) && "out of bounds");
    BufferView subview;
    subview.strides = {view.strides.begin() + view.rank(), view.strides.end()};
    subview.sizes = {view.sizes.begin() + view.rank(), view.sizes.end()};
    subview.offset = view.getPhysicalIndex(indices);
    subview.isVector = true;
    subview.numVectorDims = std::nullopt;
    return {buffer, subview};
  }

  bool operator==(const TensorOrMemref& other) const {
    if (buffer->deallocated() || other.buffer->deallocated()) return false;
    if (other.view.sizes != view.sizes) return false;
    if (other.view.numVectorDims != view.numVectorDims) return false;
    for (const auto& indices : view.indices(true)) {
      // Treat NaNs as equal.
      if constexpr (std::is_floating_point_v<T>) {
        bool thisnan = std::isnan(at(indices));
        bool othernan = std::isnan(other.at(indices));
        if (thisnan || othernan) {
          if (thisnan && othernan) continue;
          return false;
        }
      }
      if (at(indices) != other.at(indices)) return false;
    }
    return true;
  }

  std::shared_ptr<Buffer> buffer;
  BufferView view;
};

template <typename T>
struct is_tensor_or_memref : std::false_type {};  // NOLINT

template <typename T>
struct is_tensor_or_memref<TensorOrMemref<T>> : std::true_type {};  // NOLINT

template <typename T>
inline constexpr bool is_tensor_or_memref_v =  // NOLINT
    is_tensor_or_memref<std::decay_t<T>>::value;

}  // namespace interpreter
}  // namespace mlir

#endif  // MLIR_HLO_TOOLS_MLIR_INTERPRETER_FRAMEWORK_TENSOR_OR_MEMREF_H_
