/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cmath>

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

/**
 * A WindowFunctionState is a mutable, removable accumulator.
 *
 * Implementations must ensure that 'remove()' undoes 'add()' when called in FIFO order.
 * For example:
 *     'add(x); add(y); remove(x)' == 'add(y)'
 *     'add(a); add(b); add(z); remove(a); remove(b)' == 'add(z)'
 */
class WindowFunctionState {
public:
    virtual ~WindowFunctionState() = default;
    virtual void add(Value) = 0;
    virtual void remove(Value) = 0;
    virtual Value getValue() const = 0;
    virtual void reset() = 0;
};

template <AccumulatorMinMax::Sense sense>
class WindowFunctionMinMax : public WindowFunctionState {
public:
    static Value getDefault() {
        return Value{BSONNULL};
    };

    /**
     * The comparator must outlive the constructed WindowFunctionMinMax.
     */
    explicit WindowFunctionMinMax(const ValueComparator& cmp)
        : _values(cmp.makeOrderedValueMultiset()) {}

    void add(Value value) final {
        _values.insert(std::move(value));
    }

    void remove(Value value) final {
        // std::multiset::insert is guaranteed to put the element after any equal elements
        // already in the container. So find() / erase() will remove the oldest equal element,
        // which is what we want, to satisfy "remove() undoes add() when called in FIFO order".
        auto iter = _values.find(std::move(value));
        tassert(5371400, "Can't remove from an empty WindowFunctionMinMax", iter != _values.end());
        _values.erase(iter);
    }

    void reset() final {
        _values.clear();
    }

    Value getValue() const final {
        if (_values.empty())
            return getDefault();
        switch (sense) {
            case AccumulatorMinMax::Sense::kMin:
                return *_values.begin();
            case AccumulatorMinMax::Sense::kMax:
                return *_values.rbegin();
        }
        MONGO_UNREACHABLE_TASSERT(5371401);
    }

protected:
    // Holds all the values in the window, in order, with constant-time access to both ends.
    ValueMultiset _values;
};
using WindowFunctionMin = WindowFunctionMinMax<AccumulatorMinMax::Sense::kMin>;
using WindowFunctionMax = WindowFunctionMinMax<AccumulatorMinMax::Sense::kMax>;

class RemovableSum : public WindowFunctionState {
protected:
    explicit RemovableSum(ExpressionContext* const expCtx)
        : _sumAcc(AccumulatorSum::create(expCtx)),
          _posInfiniteValueCount(0),
          _negInfiniteValueCount(0),
          _nanCount(0),
          _doubleCount(0),
          _decimalCount(0) {}

public:
    static Value getDefault() {
        return Value{0};
    }

    void add(Value value) override {
        update(std::move(value), 1);
    }

    void remove(Value value) override {
        update(std::move(value), -1);
    }

    Value getValue() const override {
        if (_nanCount > 0) {
            return _decimalCount > 0 ? Value(Decimal128::kPositiveNaN)
                                     : Value(std::numeric_limits<double>::quiet_NaN());
        }
        if (_posInfiniteValueCount > 0 && _negInfiniteValueCount > 0) {
            return _decimalCount > 0 ? Value(Decimal128::kPositiveNaN)
                                     : Value(std::numeric_limits<double>::quiet_NaN());
        }
        if (_posInfiniteValueCount > 0) {
            return _decimalCount > 0 ? Value(Decimal128::kPositiveInfinity)
                                     : Value(std::numeric_limits<double>::infinity());
        }
        if (_negInfiniteValueCount > 0) {
            return _decimalCount > 0 ? Value(Decimal128::kNegativeInfinity)
                                     : Value(-std::numeric_limits<double>::infinity());
        }
        Value val = _sumAcc->getValue(false);
        if (val.getType() == NumberDouble && _doubleCount == 0 &&
            val.getDouble() > std::numeric_limits<long long>::min() &&
            val.getDouble() < std::numeric_limits<long long>::max()) {
            return Value::createIntOrLong(llround(val.getDouble()));
        }
        return _sumAcc->getValue(false);
    }

private:
    boost::intrusive_ptr<AccumulatorState> _sumAcc;
    int _posInfiniteValueCount;
    int _negInfiniteValueCount;
    int _nanCount;
    long long _doubleCount;
    long long _decimalCount;

    template <class T>
    void accountForIntegral(T value, int quantity) {
        if (value == std::numeric_limits<T>::min() && quantity == -1) {
            // Avoid overflow by processing in two parts.
            _sumAcc->process(Value(std::numeric_limits<T>::max()), false);
            _sumAcc->process(Value(1), false);
        } else {
            _sumAcc->process(Value(value * quantity), false);
        }
    }

    void accountForDouble(double value, int quantity) {
        // quantity should be 1 if adding value, -1 if removing value
        if (std::isnan(value)) {
            _nanCount += quantity;
        } else if (value == std::numeric_limits<double>::infinity()) {
            _posInfiniteValueCount += quantity;
        } else if (value == -std::numeric_limits<double>::infinity()) {
            _negInfiniteValueCount += quantity;
        } else {
            _sumAcc->process(Value(value * quantity), false);
        }
    }

    void accountForDecimal(Decimal128 value, int quantity) {
        // quantity should be 1 if adding value, -1 if removing value
        if (value.isNaN()) {
            _nanCount += quantity;
        } else if (value.isInfinite() && !value.isNegative()) {
            _posInfiniteValueCount += quantity;
        } else if (value.isInfinite() && value.isNegative()) {
            _negInfiniteValueCount += quantity;
        } else {
            if (quantity == -1) {
                value = value.negate();
            }
            _sumAcc->process(Value(value), false);
        }
    }

    void update(Value value, int quantity) {
        // quantity should be 1 if adding value, -1 if removing value
        switch (value.getType()) {
            case NumberInt:
                accountForIntegral(value.getInt(), quantity);
                break;
            case NumberLong:
                accountForIntegral(value.getLong(), quantity);
                break;
            case NumberDouble:
                _doubleCount += quantity;
                accountForDouble(value.getDouble(), quantity);
                break;
            case NumberDecimal:
                _decimalCount += quantity;
                accountForDecimal(value.getDecimal(), quantity);
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(5371300);
        }
    }
};

class WindowFunctionSum final : public RemovableSum {
public:
    explicit WindowFunctionSum(ExpressionContext* const expCtx) : RemovableSum(expCtx) {}
};

class WindowFunctionAvg final : public RemovableSum {
public:
    explicit WindowFunctionAvg(ExpressionContext* const expCtx) : RemovableSum(expCtx), _count(0) {}

    static Value getDefault() {
        return Value(BSONNULL);
    }

    void add(Value value) final {
        RemovableSum::add(std::move(value));
        _count++;
    }

    void remove(Value value) final {
        RemovableSum::remove(std::move(value));
        _count--;
    }

    Value getValue() const final {
        if (_count == 0) {
            return getDefault();
        }
        Value sum = RemovableSum::getValue();
        switch (sum.getType()) {
            case NumberInt:
            case NumberLong:
                return Value(sum.coerceToDouble() / static_cast<double>(_count));
            case NumberDouble: {
                double internalSum = sum.getDouble();
                if (std::isnan(internalSum) || std::isinf(internalSum)) {
                    return sum;
                }
                return Value(internalSum / static_cast<double>(_count));
            }
            case NumberDecimal: {
                Decimal128 internalSum = sum.getDecimal();
                if (internalSum.isNaN() || internalSum.isInfinite()) {
                    return sum;
                }
                return Value(internalSum.divide(Decimal128(_count)));
            }
            default:
                MONGO_UNREACHABLE_TASSERT(5371301);
        }
    }

private:
    long long _count;
};
}  // namespace mongo
