#pragma once

#include <AggregateFunctions/ReservoirSampler.h>

#include <Common/FieldVisitors.h>

#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>

#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeArray.h>

#include <AggregateFunctions/IUnaryAggregateFunction.h>

#include <Columns/ColumnArray.h>
#include <Columns/ColumnsNumber.h>


namespace DB
{

template <typename ArgumentFieldType>
struct AggregateFunctionQuantileData
{
    using Sample = ReservoirSampler<ArgumentFieldType, ReservoirSamplerOnEmpty::RETURN_NAN_OR_ZERO>;
    Sample sample;  /// TODO Add MemoryTracker
};


/** Approximately calculates the quantile.
  * The argument type can only be a numeric type (including date and date-time).
  * If returns_float = true, the result type is Float64, otherwise - the result type is the same as the argument type.
  * For dates and date-time, returns_float should be set to false.
  */
template <typename ArgumentFieldType, bool returns_float = true>
class AggregateFunctionQuantile final
    : public IUnaryAggregateFunction<AggregateFunctionQuantileData<ArgumentFieldType>, AggregateFunctionQuantile<ArgumentFieldType, returns_float>>
{
private:
    using Sample = typename AggregateFunctionQuantileData<ArgumentFieldType>::Sample;

    double level;
    DataTypePtr type;

public:
    AggregateFunctionQuantile(double level_ = 0.5) : level(level_) {}

    String getName() const override { return "quantile"; }

    DataTypePtr getReturnType() const override
    {
        return type;
    }

    void setArgument(const DataTypePtr & argument)
    {
        if (returns_float)
            type = std::make_shared<DataTypeFloat64>();
        else
            type = argument;
    }

    void setParameters(const Array & params) override
    {
        if (params.size() != 1)
            throw Exception("Aggregate function " + getName() + " requires exactly one parameter.", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        level = applyVisitor(FieldVisitorConvertToNumber<Float64>(), params[0]);
    }


    void addImpl(AggregateDataPtr place, const IColumn & column, size_t row_num, Arena *) const
    {
        this->data(place).sample.insert(static_cast<const ColumnVector<ArgumentFieldType> &>(column).getData()[row_num]);
    }

    void merge(AggregateDataPtr place, ConstAggregateDataPtr rhs, Arena * arena) const override
    {
        this->data(place).sample.merge(this->data(rhs).sample);
    }

    void serialize(ConstAggregateDataPtr place, WriteBuffer & buf) const override
    {
        this->data(place).sample.write(buf);
    }

    void deserialize(AggregateDataPtr place, ReadBuffer & buf, Arena *) const override
    {
        this->data(place).sample.read(buf);
    }

    void insertResultInto(ConstAggregateDataPtr place, IColumn & to) const override
    {
        /// `Sample` can be sorted when a quantile is received, but in this context, you can not think of this as a violation of constancy.
        Sample & sample = const_cast<Sample &>(this->data(place).sample);

        if (returns_float)
            static_cast<ColumnFloat64 &>(to).getData().push_back(sample.quantileInterpolated(level));
        else
            static_cast<ColumnVector<ArgumentFieldType> &>(to).getData().push_back(sample.quantileInterpolated(level));
    }

    const char * getHeaderFilePath() const override { return __FILE__; }
};


/** The same, but allows you to calculate several quantiles at once.
  * For this, takes as parameters several levels. Example: quantiles(0.5, 0.8, 0.9, 0.95)(ConnectTiming).
  * Returns an array of results.
  */
template <typename ArgumentFieldType, bool returns_float = true>
class AggregateFunctionQuantiles final
    : public IUnaryAggregateFunction<AggregateFunctionQuantileData<ArgumentFieldType>, AggregateFunctionQuantiles<ArgumentFieldType, returns_float>>
{
private:
    using Sample = typename AggregateFunctionQuantileData<ArgumentFieldType>::Sample;

    using Levels = std::vector<double>;
    Levels levels;
    DataTypePtr type;

public:
    String getName() const override { return "quantiles"; }

    DataTypePtr getReturnType() const override
    {
        return std::make_shared<DataTypeArray>(type);
    }

    void setArgument(const DataTypePtr & argument)
    {
        if (returns_float)
            type = std::make_shared<DataTypeFloat64>();
        else
            type = argument;
    }

    void setParameters(const Array & params) override
    {
        if (params.empty())
            throw Exception("Aggregate function " + getName() + " requires at least one parameter.", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        size_t size = params.size();
        levels.resize(size);

        for (size_t i = 0; i < size; ++i)
            levels[i] = applyVisitor(FieldVisitorConvertToNumber<Float64>(), params[i]);
    }


    void addImpl(AggregateDataPtr place, const IColumn & column, size_t row_num, Arena *) const
    {
        this->data(place).sample.insert(static_cast<const ColumnVector<ArgumentFieldType> &>(column).getData()[row_num]);
    }

    void merge(AggregateDataPtr place, ConstAggregateDataPtr rhs, Arena * arena) const override
    {
        this->data(place).sample.merge(this->data(rhs).sample);
    }

    void serialize(ConstAggregateDataPtr place, WriteBuffer & buf) const override
    {
        this->data(place).sample.write(buf);
    }

    void deserialize(AggregateDataPtr place, ReadBuffer & buf, Arena *) const override
    {
        this->data(place).sample.read(buf);
    }

    void insertResultInto(ConstAggregateDataPtr place, IColumn & to) const override
    {
        /// `Sample` can be sorted when a quantile is received, but in this context, you can not think of this as a violation of constancy.
        Sample & sample = const_cast<Sample &>(this->data(place).sample);

        ColumnArray & arr_to = static_cast<ColumnArray &>(to);
        ColumnArray::Offsets_t & offsets_to = arr_to.getOffsets();

        size_t size = levels.size();
        offsets_to.push_back((offsets_to.size() == 0 ? 0 : offsets_to.back()) + size);

        if (returns_float)
        {
            ColumnFloat64::Container_t & data_to = static_cast<ColumnFloat64 &>(arr_to.getData()).getData();

            for (size_t i = 0; i < size; ++i)
                data_to.push_back(sample.quantileInterpolated(levels[i]));
        }
        else
        {
            typename ColumnVector<ArgumentFieldType>::Container_t & data_to = static_cast<ColumnVector<ArgumentFieldType> &>(arr_to.getData()).getData();

            for (size_t i = 0; i < size; ++i)
                data_to.push_back(sample.quantileInterpolated(levels[i]));
        }
    }

    const char * getHeaderFilePath() const override { return __FILE__; }
};

}
