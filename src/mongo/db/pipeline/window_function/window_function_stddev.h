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

#include "mongo/db/pipeline/window_function/window_function.h"

namespace mongo {

class WindowFunctionStdDev : public WindowFunctionState {
protected:
    explicit WindowFunctionStdDev(ExpressionContext* const expCtx, bool isSamp)
        : WindowFunctionState(expCtx),
          _mean(AccumulatorSum::create(expCtx)),
          _m2(AccumulatorSum::create(expCtx)),
          _isSamp(isSamp),
          _count(0),
          _nonfiniteValueCount(0) {}

public:
    static Value getDefault() {
        return Value(BSONNULL);
    }

    void add(Value value) {
        if (accountForNonfinite(value, +1))
            return;
        _count++;
        double delta = value.coerceToDouble() - _mean->getValue(false).coerceToDouble();
        _mean->process(Value{delta / _count}, false);
        _m2->process(Value(delta * (value.coerceToDouble() - _mean->getValue(false).coerceToDouble())), false);
    }

    void remove(Value value) {
        if (accountForNonfinite(value, -1))
            return;
        if (_count == 1) {
            reset();
            return;
        }
        double val = value.coerceToDouble();
        double old_mean = _mean->getValue(false).coerceToDouble();
        double new_mean = (old_mean * _count - val) / (_count - 1);
        _mean->process(Value{new_mean - old_mean}, false);
        _m2->process(Value((new_mean - val) * (val - old_mean)), false);
        _count--;
    }

    Value getValue() const final {
        if (_nonfiniteValueCount > 0)
            return Value(std::numeric_limits<double>::quiet_NaN());
        const long long adjustedCount = _isSamp ? _count - 1 : _count;
        if (adjustedCount == 0)
            return getDefault();
        return Value(sqrt(_m2->getValue(false).coerceToDouble() / adjustedCount));
    }

    void reset() {
        _m2->reset();
        _mean->reset();
        _count = 0;
        _nonfiniteValueCount = 0;
    }

private:
    // Returns true if value is nonfinite and it has been accounted for.
    bool accountForNonfinite(Value value, int quantity) {
        if (!value.numeric())
            return true;
        if ((value.getType() == NumberDouble && !std::isfinite(value.getDouble())) ||
            (value.getType() == NumberDecimal && !value.getDecimal().isFinite())) {
            _nonfiniteValueCount += quantity;
            _count += quantity;
            return true;
        }
        return false;
    }

    // Std dev cannot make use of RemovableSum because of its specific handling of non-finite
    // values. Adding a NaN or +/-inf makes the result NaN. Additionally, the consistent and
    // exclusive use of doubles in std dev calculations makes the type handling in RemovableSum
    // unnecessary.
    boost::intrusive_ptr<AccumulatorState> _mean;
    boost::intrusive_ptr<AccumulatorState> _m2;
    bool _isSamp;
    long long _count;
    int _nonfiniteValueCount;
};

class WindowFunctionStdDevPop final : public WindowFunctionStdDev {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionStdDevPop>(expCtx);
    }
    explicit WindowFunctionStdDevPop(ExpressionContext* const expCtx)
        : WindowFunctionStdDev(expCtx, false) {}
};

class WindowFunctionStdDevSamp final : public WindowFunctionStdDev {
public:
    static std::unique_ptr<WindowFunctionState> create(ExpressionContext* const expCtx) {
        return std::make_unique<WindowFunctionStdDevSamp>(expCtx);
    }
    explicit WindowFunctionStdDevSamp(ExpressionContext* const expCtx)
        : WindowFunctionStdDev(expCtx, true) {}
};
}  // namespace mongo
