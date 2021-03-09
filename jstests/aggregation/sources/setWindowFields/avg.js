/**
 * Test that $avg works as a window function.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1}))
        .featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

// Run the suite of partition and bounds tests against the $avg function.
// TODO SERVER-53713 Enable removable testing.
testAccumAgainstGroup(coll, "$avg", null, true);

// Test a combination of two different runnning averages.
let results =
    coll.aggregate([
            {
                $setWindowFields: {
                    sortBy: {_id: 1},
                    partitionBy: "$ticker",
                    output: {
                        runningAvg: {$avg: "$price", window: {documents: ["unbounded", "current"]}},
                        runningAvgLead: {$avg: "$price", window: {documents: ["unbounded", 3]}},
                    }
                },
            },
        ])
        .toArray();
})();
