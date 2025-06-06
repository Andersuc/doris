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

import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.awaitility.Awaitility

suite("test_index_compaction_p0", "p0, nonConcurrent") {

    def compaction_table_name = "test_index_compaction_p0_httplogs"

    def load_json_data = {table_name, file_name ->
        // load the json data
        streamLoad {
            table "${table_name}"

            // set http request header params
            set 'read_json_by_line', 'true'
            set 'format', 'json'
            set 'max_filter_ratio', '0.1'
            file file_name // import json file
            time 10000 // limit inflight 10s

            // if declared a check callback, the default check condition will ignore.
            // So you must check all condition

            check { result, exception, startTime, endTime ->
                if (exception != null) {
                        throw exception
                }
                logger.info("Stream load ${file_name} result: ${result}".toString())
                def json = parseJson(result)
                assertEquals("success", json.Status.toLowerCase())
                // assertEquals(json.NumberTotalRows, json.NumberLoadedRows + json.NumberUnselectedRows)
                assertTrue(json.NumberLoadedRows > 0 && json.LoadBytes > 0)
            }
        }
    }
    sql "DROP TABLE IF EXISTS ${compaction_table_name}"
    sql """
        CREATE TABLE ${compaction_table_name} (
            `@timestamp` int(11) NULL,
            `clientip` varchar(20) NULL,
            `request` varchar(500) NULL,
            `status` int NULL,
            `size` int NULL,
            INDEX clientip_idx (`clientip`) USING INVERTED COMMENT '',
            INDEX request_idx (`request`) USING INVERTED PROPERTIES("parser" = "unicode") COMMENT '',
            INDEX status_idx (`status`) USING INVERTED COMMENT '',
            INDEX size_idx (`size`) USING INVERTED COMMENT ''
        ) ENGINE=OLAP
            DISTRIBUTED BY HASH(`@timestamp`) BUCKETS 1
            PROPERTIES (
            "replication_allocation" = "tag.location.default: 1",
            "compaction_policy" = "time_series",
            "time_series_compaction_file_count_threshold" = "10",
            "disable_auto_compaction" = "true"
        );
    """
    (1..20).each { i ->
        def fileName = "documents-" + i + ".json"
        load_json_data.call(compaction_table_name, """${fileName}""")
    }

    def backendId_to_backendIP = [:]
    def backendId_to_backendHttpPort = [:]
    getBackendIpHttpPort(backendId_to_backendIP, backendId_to_backendHttpPort);

    def set_be_config = { key, value ->
        for (String backend_id: backendId_to_backendIP.keySet()) {
            def (code, out, err) = update_be_config(backendId_to_backendIP.get(backend_id), backendId_to_backendHttpPort.get(backend_id), key, value)
            logger.info("update config: code=" + code + ", out=" + out + ", err=" + err)
        }
    }
    set_be_config.call("inverted_index_compaction_enable", "true")
    //TabletId,ReplicaId,BackendId,SchemaHash,Version,LstSuccessVersion,LstFailedVersion,LstFailedTime,LocalDataSize,RemoteDataSize,RowCount,State,LstConsistencyCheckTime,CheckVersion,VersionCount,QueryHits,PathHash,MetaUrl,CompactionStatus
    def tablets = sql_return_maparray """ show tablets from ${compaction_table_name}; """


    // Do not check tablets rowset count before compaction
    /*for (def tablet in tablets) {
        int beforeSegmentCount = 0
        String tablet_id = tablet.TabletId
        (code, out, err) = curl("GET", tablet.CompactionStatus)
        logger.info("Show tablets status: code=" + code + ", out=" + out + ", err=" + err)
        assertEquals(code, 0)
        def tabletJson = parseJson(out.trim())
        assert tabletJson.rowsets instanceof List
        for (String rowset in (List<String>) tabletJson.rowsets) {
            beforeSegmentCount += Integer.parseInt(rowset.split(" ")[1])
        }
        assertEquals(beforeSegmentCount, 20)
    }*/

    // trigger compactions for all tablets in ${tableName}
    trigger_and_wait_compaction(compaction_table_name, "full")
    for (def tablet in tablets) {
        int afterSegmentCount = 0
        String tablet_id = tablet.TabletId
        def (code, out, err) = curl("GET", tablet.CompactionStatus)
        logger.info("Show tablets status: code=" + code + ", out=" + out + ", err=" + err)
        assertEquals(code, 0)
        def tabletJson = parseJson(out.trim())
        assert tabletJson.rowsets instanceof List
        for (String rowset in (List<String>) tabletJson.rowsets) {
            logger.info("rowset is: " + rowset)
            afterSegmentCount += Integer.parseInt(rowset.split(" ")[1])
        }
        assertEquals(afterSegmentCount, 1)
    }

}
