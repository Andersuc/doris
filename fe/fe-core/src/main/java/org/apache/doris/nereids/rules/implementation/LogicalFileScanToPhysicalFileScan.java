// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.nereids.rules.implementation;

import org.apache.doris.nereids.properties.DistributionSpecAny;
import org.apache.doris.nereids.rules.Rule;
import org.apache.doris.nereids.rules.RuleType;
import org.apache.doris.nereids.trees.plans.logical.LogicalHudiScan;
import org.apache.doris.nereids.trees.plans.physical.PhysicalFileScan;

import java.util.Optional;

/**
 * Implementation rule that convert logical FileScan to physical FileScan.
 */
public class LogicalFileScanToPhysicalFileScan extends OneImplementationRuleFactory {
    @Override
    public Rule build() {
        return logicalFileScan().when(plan -> !(plan instanceof LogicalHudiScan)).then(fileScan ->
            new PhysicalFileScan(
                    fileScan.getRelationId(),
                    fileScan.getTable(),
                    fileScan.getQualifier(),
                    DistributionSpecAny.INSTANCE,
                    Optional.empty(),
                    fileScan.getLogicalProperties(),
                    fileScan.getSelectedPartitions(),
                    fileScan.getTableSample(),
                    fileScan.getTableSnapshot(),
                    fileScan.getOperativeSlots(),
                    fileScan.getScanParams())
        ).toRule(RuleType.LOGICAL_FILE_SCAN_TO_PHYSICAL_FILE_SCAN_RULE);
    }
}
