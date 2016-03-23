/*
 * test_NDPluginTimeSeries.cpp
 *
 *  Created on: 21 Mar 2016
 *      Author: Ulrik Pedersen
 */

#include <stdio.h>


#include "boost/test/unit_test.hpp"

// AD dependencies
#include <NDPluginDriver.h>
#include <NDArray.h>
#include <NDAttribute.h>
#include <asynDriver.h>

#include <string.h>
#include <stdint.h>

#include <deque>
#include <tr1/memory>
#include <iostream>
#include <fstream>
using namespace std;

#include "testingutilities.h"
#include "TimeSeriesPluginWrapper.h"
#include "AsynException.h"


static int callbackCount = 0;
static void *cbPtr = 0;

void TS_callback(void *userPvt, asynUser *pasynUser, void *pointer)
{
  cbPtr = pointer;
  callbackCount++;
}

struct TimeSeriesPluginTestFixture
{
  NDArrayPool *arrayPool;
  std::tr1::shared_ptr<asynPortDriver> driver;
  std::tr1::shared_ptr<TimeSeriesPluginWrapper> ts;
  std::tr1::shared_ptr<asynGenericPointerClient> client;
  std::vector<NDArray*>arrays_1d;
  std::vector<size_t>dims_1d;
  std::vector<NDArray*>arrays_2d;
  std::vector<size_t>dims_2d;
  std::vector<NDArray*>arrays_3d;
  std::vector<size_t>dims_3d;

  static int testCase;

  TimeSeriesPluginTestFixture()
  {
    arrayPool = new NDArrayPool(100, 0);

    // Asyn manager doesn't like it if we try to reuse the same port name for multiple drivers (even if only one is ever instantiated at once), so
    // change it slightly for each test case.
    std::string simport("simTimeSeriesTest"), testport("TS");
    uniqueAsynPortName(simport);
    uniqueAsynPortName(testport);

    // We need some upstream driver for our test plugin so that calls to connectArrayPort
    // don't fail, but we can then ignore it and send arrays by calling processCallbacks directly.
    driver = std::tr1::shared_ptr<asynPortDriver>(new asynPortDriver(simport.c_str(),
                                                                     0, 1,
                                                                     asynGenericPointerMask,
                                                                     asynGenericPointerMask,
                                                                     0, 0, 0, 2000000));

    // This is the plugin under test
    ts = std::tr1::shared_ptr<TimeSeriesPluginWrapper>(new TimeSeriesPluginWrapper(testport.c_str(),
                                                                      50,
                                                                      1,
                                                                      simport.c_str(),
                                                                      0,
                                                                      1,
                                                                      0,
                                                                      0,
                                                                      2000000));

    // Enable the plugin
    ts->write(NDPluginDriverEnableCallbacksString, 1);
    ts->write(NDPluginDriverBlockingCallbacksString, 1);

    client = std::tr1::shared_ptr<asynGenericPointerClient>(new asynGenericPointerClient(testport.c_str(), 0, NDArrayDataString));
    client->registerInterruptUser(&TS_callback);

    // 1D: A single channel with 20 time series elements
    size_t tmpdims_1d[] = {20};
    dims_1d.assign(tmpdims_1d, tmpdims_1d + sizeof(tmpdims_1d)/sizeof(tmpdims_1d[0]));
    arrays_1d.resize(24);

    // 2D: two time series channels, each with 20 elements
    size_t tmpdims_2d[] = {2,20};
    dims_2d.assign(tmpdims_2d, tmpdims_2d + sizeof(tmpdims_2d)/sizeof(tmpdims_2d[0]));
    arrays_2d.resize(24);

    // 3D: three channels with 2D images of 4x5 pixel (like an RGB image)
    // Not valid input for the Time Series plugin
    size_t tmpdims_3d[] = {3,4,5};
    dims_3d.assign(tmpdims_3d, tmpdims_3d + sizeof(tmpdims_3d)/sizeof(tmpdims_3d[0]));
    arrays_3d.resize(24);
}

  ~TimeSeriesPluginTestFixture()
  {
    delete arrayPool;
    client.reset();
    ts.reset();
    driver.reset();
  }

};

BOOST_FIXTURE_TEST_SUITE(TimeSeriesPluginTests, TimeSeriesPluginTestFixture)

BOOST_AUTO_TEST_CASE(invalid_number_dimensions)
{
  fillNDArrays(dims_3d, NDFloat32, arrays_3d);
  BOOST_REQUIRE_EQUAL(arrays_3d[0]->ndims, 3);

  // Actually the processCallbacks() function returns void and do not generally
  // throw exceptions. The ideal testing is to check for an exception but alas
  // we cannot do that here. Thus we comment out the ideal test:
  // BOOST_CHECK_THROW(ts->processCallbacks(arrays[0]), AsynException);

  // At least we can test that it doesn't crash...
  BOOST_MESSAGE("Expecting stdout message \"error, number of array dimensions...\"");
  BOOST_CHECK_NO_THROW(ts->processCallbacks(arrays_3d[0]));   // non-ideal test
  BOOST_CHECK_EQUAL(ts->readInt(TSCurrentPointString), 0);    // non-ideal test
}


BOOST_AUTO_TEST_CASE(basic_1D_operation)
{
  // Fill some NDArrays with unimportant data
  fillNDArrays(dims_1d, NDFloat32, arrays_1d);

  BOOST_MESSAGE("Testing 1D input arrays: " << arrays_1d[0]->dims[0].size
                << " elements. Averaging=" << 10 << " Time series length=" << 20);

  // Double check one of the NDArrays dimensions and datatype
  BOOST_REQUIRE_EQUAL(arrays_1d[0]->ndims, 1);
  BOOST_CHECK_EQUAL(arrays_1d[0]->dims[0].size, 20);
  BOOST_CHECK_EQUAL(arrays_1d[0]->dataType, NDFloat32);

  // Plugin setup
  BOOST_CHECK_NO_THROW(ts->write(TSTimePerPointString, 0.001));
  BOOST_CHECK_NO_THROW(ts->write(TSAveragingTimeString, 0.01));
  BOOST_CHECK_NO_THROW(ts->write(TSAcquireModeString, 0)); // TSAcquireModeFixed=0
  BOOST_CHECK_NO_THROW(ts->write(TSNumPointsString, 20));

  // Double check plugin setup
  BOOST_CHECK_EQUAL(ts->readInt(TSNumAverageString), 10);
  BOOST_CHECK_EQUAL(ts->readInt(TSNumPointsString), 20);

  BOOST_CHECK_NO_THROW(ts->write(TSAcquireString, 1));
  BOOST_CHECK_EQUAL(ts->readInt(TSAcquireString), 1);

  // Process 10 arrays through the TS plugin. As we have averaged by 10 TimePoints
  // (see TSNumAverage) we should then have a new 20 point Time Series output.
  for (int i = 0; i < 10; i++)
  {
    ts->lock();
    BOOST_CHECK_NO_THROW(ts->processCallbacks(arrays_1d[i]));
    ts->unlock();
    BOOST_CHECK_EQUAL(ts->readInt(TSCurrentPointString), (i+1)*2); // num points in NDArray timeseries / NumAverage
  }
  // As we are using Fixed Lenght mode, acqusition should now have stopped
  BOOST_REQUIRE_EQUAL(ts->readInt(TSAcquireString), 0);
}

BOOST_AUTO_TEST_CASE(basic_2D_operation)
{
  // Fill some NDArrays with unimportant data
  fillNDArrays(dims_2d, NDFloat32, arrays_2d);

  BOOST_MESSAGE("Testing 2D input arrays: " << arrays_2d[0]->dims[0].size
                << " channels with " << arrays_2d[0]->dims[1].size
                << " elements. Averaging=" << 10 << " Time series length=" << 20);

  // Double check one of the NDArrays dimensions and datatype
  BOOST_REQUIRE_EQUAL(arrays_2d[0]->ndims, 2);
  BOOST_CHECK_EQUAL(arrays_2d[0]->dims[0].size, 2);
  BOOST_CHECK_EQUAL(arrays_2d[0]->dims[1].size, 20);
  BOOST_CHECK_EQUAL(arrays_2d[0]->dataType, NDFloat32);

  // Plugin setup
  BOOST_CHECK_NO_THROW(ts->write(TSTimePerPointString, 0.001));
  BOOST_CHECK_NO_THROW(ts->write(TSAveragingTimeString, 0.01));
  BOOST_CHECK_NO_THROW(ts->write(TSAcquireModeString, 0)); // TSAcquireModeFixed=0
  BOOST_CHECK_NO_THROW(ts->write(TSNumPointsString, 20));

  // Double check plugin setup
  BOOST_CHECK_EQUAL(ts->readInt(TSNumAverageString), 10);
  BOOST_CHECK_EQUAL(ts->readInt(TSNumPointsString), 20);

  BOOST_CHECK_NO_THROW(ts->write(TSAcquireString, 1));
  BOOST_CHECK_EQUAL(ts->readInt(TSAcquireString), 1);

  // Process 10 arrays through the TS plugin. As we have averaged by 10 TimePoints
  // (see TSNumAverage) we should then have a new 20 point Time Series output.
  for (int i = 0; i < 10; i++)
  {
    ts->lock();
    BOOST_CHECK_NO_THROW(ts->processCallbacks(arrays_2d[i]));
    ts->unlock();
    BOOST_CHECK_EQUAL(ts->readInt(TSCurrentPointString), (i+1)*2); // num points in NDArray timeseries / NumAverage
  }

  // As we are using Fixed Length mode, acquisition should now have stopped
  BOOST_CHECK_EQUAL(ts->readInt(TSAcquireString), 0);

  // Processing an extra array through should not have an effect as acquisition
  // has stopped
  BOOST_CHECK_NO_THROW(ts->processCallbacks(arrays_2d[10]));
  BOOST_CHECK_EQUAL(ts->readInt(TSCurrentPointString), 20);
}

BOOST_AUTO_TEST_CASE(circular_2D_operation)
{
  // Fill some NDArrays with unimportant data
  fillNDArrays(dims_2d, NDFloat32, arrays_2d);

  BOOST_MESSAGE("Testing 2D input arrays: " << arrays_2d[0]->dims[0].size
                << " channels with " << arrays_2d[0]->dims[1].size
                << " elements. Averaging=" << 10 << " Time series length=" << 20);

  // Double check one of the NDArrays dimensions and datatype
  BOOST_REQUIRE_EQUAL(arrays_2d[0]->ndims, 2);
  BOOST_CHECK_EQUAL(arrays_2d[0]->dims[0].size, 2);
  BOOST_CHECK_EQUAL(arrays_2d[0]->dims[1].size, 20);
  BOOST_CHECK_EQUAL(arrays_2d[0]->dataType, NDFloat32);

  // Plugin setup
  BOOST_CHECK_NO_THROW(ts->write(TSTimePerPointString, 0.001));
  BOOST_CHECK_NO_THROW(ts->write(TSAveragingTimeString, 0.01));
  BOOST_CHECK_NO_THROW(ts->write(TSAcquireModeString, 1)); // TSAcquireModeCircular=1
  BOOST_CHECK_NO_THROW(ts->write(TSNumPointsString, 20));

  // Double check plugin setup
  BOOST_CHECK_EQUAL(ts->readInt(TSNumAverageString), 10);
  BOOST_CHECK_EQUAL(ts->readInt(TSNumPointsString), 20);

  BOOST_CHECK_NO_THROW(ts->write(TSAcquireString, 1));
  BOOST_CHECK_EQUAL(ts->readInt(TSAcquireString), 1);

  // Process 9 arrays through the TS plugin.
  for (int i = 0; i < 9; i++)
  {
    ts->lock();
    BOOST_CHECK_NO_THROW(ts->processCallbacks(arrays_2d[i]));
    ts->unlock();
    BOOST_CHECK_EQUAL(ts->readInt(TSCurrentPointString), (i+1)*2); // num points in NDArray timeseries / NumAverage
  }

  // Process 10th array through the TS plugin
  BOOST_CHECK_NO_THROW(ts->processCallbacks(arrays_2d[9]));
  // The current point should now have been reset to 0 as the buffer has filled
  // up and wrapped around
  BOOST_CHECK_EQUAL(ts->readInt(TSCurrentPointString), 0);

  BOOST_CHECK_EQUAL(ts->readInt(TSAcquireString), 1);

  // Process 11th array through the TS plugin
  BOOST_CHECK_NO_THROW(ts->processCallbacks(arrays_2d[10]));
  BOOST_CHECK_EQUAL(ts->readInt(TSCurrentPointString), 2);
  BOOST_CHECK_EQUAL(ts->readInt(TSAcquireString), 1);
}


BOOST_AUTO_TEST_SUITE_END() // Done!
