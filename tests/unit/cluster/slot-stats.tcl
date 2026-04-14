# Integration tests for CLUSTER SLOT-STATS command.

# -----------------------------------------------------------------------------
# Helper functions for CLUSTER SLOT-STATS test cases.
# -----------------------------------------------------------------------------

# Converts array RESP response into a dict.
# This is useful for many test cases, where unnecessary nesting is removed.
proc convert_array_into_dict {slot_stats} {
    set res [dict create]
    foreach slot_stat $slot_stats {
        # slot_stat is an array of size 2, where 0th index represents (int) slot, 
        # and 1st index represents (map) usage statistics.
        dict set res [lindex $slot_stat 0] [lindex $slot_stat 1]
    }
    return $res
}

proc get_cmdstat_usec {cmd r} {
    set cmdstatline [cmdrstat $cmd r]
    regexp "usec=(.*?),usec_per_call=(.*?),rejected_calls=0,failed_calls=0" $cmdstatline -> usec _
    return $usec
}

proc initialize_expected_slots_dict {} {
    set expected_slots [dict create]
    for {set i 0} {$i < 16384} {incr i 1} {
        dict set expected_slots $i 0
    }
    return $expected_slots
}

proc initialize_expected_slots_dict_with_range {start_slot end_slot} {
    assert {$start_slot <= $end_slot}
    set expected_slots [dict create]
    for {set i $start_slot} {$i <= $end_slot} {incr i 1} {
        dict set expected_slots $i 0
    }
    return $expected_slots
}

proc assert_empty_slot_stats {slot_stats metrics_to_assert} {
    set slot_stats [convert_array_into_dict $slot_stats]
    dict for {slot stats} $slot_stats {
        foreach metric_name $metrics_to_assert {
            set metric_value [dict get $stats $metric_name]
            assert {$metric_value == 0}
        }
    }
}

proc assert_empty_slot_stats_with_exception {slot_stats exception_slots metrics_to_assert} {
    set slot_stats [convert_array_into_dict $slot_stats]
    dict for {slot stats} $exception_slots {
        assert {[dict exists $slot_stats $slot]} ;# slot_stats must contain the expected slots.
    }
    dict for {slot stats} $slot_stats {
        if {[dict exists $exception_slots $slot]} {
            foreach metric_name $metrics_to_assert {
                set metric_value [dict get $exception_slots $slot $metric_name]
                assert {[dict get $stats $metric_name] == $metric_value}
            }
        } else {
            dict for {metric value} $stats {
                assert {$value == 0}
            }
        }
    }
}

proc assert_equal_slot_stats {slot_stats_1 slot_stats_2 deterministic_metrics non_deterministic_metrics} {
    set slot_stats_1 [convert_array_into_dict $slot_stats_1]
    set slot_stats_2 [convert_array_into_dict $slot_stats_2]
    assert {[dict size $slot_stats_1] == [dict size $slot_stats_2]}

    dict for {slot stats_1} $slot_stats_1 {
        assert {[dict exists $slot_stats_2 $slot]}
        set stats_2 [dict get $slot_stats_2 $slot]

        # For deterministic metrics, we assert their equality.
        foreach metric $deterministic_metrics {
            assert {[dict get $stats_1 $metric] == [dict get $stats_2 $metric]}
        }
        # For non-deterministic metrics, we assert their non-zeroness as a best-effort.
        foreach metric $non_deterministic_metrics {
            assert {([dict get $stats_1 $metric] == 0 && [dict get $stats_2 $metric] == 0) || \
                    ([dict get $stats_1 $metric] != 0 && [dict get $stats_2 $metric] != 0)}
        }
    }
}

proc assert_all_slots_have_been_seen {expected_slots} {
    dict for {k v} $expected_slots {
        assert {$v == 1}
    }
}

proc assert_slot_visibility {slot_stats expected_slots} {
    set slot_stats [convert_array_into_dict $slot_stats]
    dict for {slot _} $slot_stats {
        assert {[dict exists $expected_slots $slot]}
        dict set expected_slots $slot 1
    }

    assert_all_slots_have_been_seen $expected_slots
}

proc assert_slot_stats_monotonic_order {slot_stats orderby is_desc} {
    # For Tcl dict, the order of iteration is the order in which the keys were inserted into the dictionary
    # Thus, the response ordering is preserved upon calling 'convert_array_into_dict()'.
    # Source: https://www.tcl.tk/man/tcl8.6.11/TclCmd/dict.htm
    set slot_stats [convert_array_into_dict $slot_stats]
    set prev_metric -1
    dict for {_ stats} $slot_stats {
        set curr_metric [dict get $stats $orderby]
        if {$prev_metric != -1} {
            if {$is_desc == 1} {
                assert {$prev_metric >= $curr_metric}
            } else {
                assert {$prev_metric <= $curr_metric}
            }
        }
        set prev_metric $curr_metric
    }
}

proc assert_slot_stats_monotonic_descent {slot_stats orderby} {
    assert_slot_stats_monotonic_order $slot_stats $orderby 1
}

proc assert_slot_stats_monotonic_ascent {slot_stats orderby} {
    assert_slot_stats_monotonic_order $slot_stats $orderby 0
}

proc wait_for_replica_key_exists {key key_count} {
    wait_for_condition 1000 50 {
        [R 1 exists $key] eq "$key_count"
    } else {
        fail "Test key was not replicated"
    }
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS cpu-usec metric correctness.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {

    # Define shared variables.
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]
    set key_secondary "FOO2"
    set key_secondary_slot [R 0 cluster keyslot $key_secondary]
    set metrics_to_assert [list cpu-usec]

    test "CLUSTER SLOT-STATS cpu-usec reset upon CONFIG RESETSTAT." {
        R 0 SET $key VALUE
        R 0 DEL $key
        R 0 CONFIG RESETSTAT
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec reset upon slot migration." {
        R 0 SET $key VALUE

        R 0 CLUSTER DELSLOTS $key_slot
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert

        R 0 CLUSTER ADDSLOTS $key_slot
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for non-slot specific commands." {
        R 0 INFO
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for slot specific commands." {
        R 0 SET $key VALUE
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set usec [get_cmdstat_usec set r]
        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec $usec
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for blocking commands, unblocked on keyspace update." {
        # Blocking command with no timeout. Only keyspace update can unblock this client.
        set rd [valkey_deferring_client]
        $rd BLPOP $key 0
        wait_for_blocked_clients_count 1
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        # When the client is blocked, no accumulation is made. This behaviour is identical to INFO COMMANDSTATS.
        assert_empty_slot_stats $slot_stats $metrics_to_assert

        # Unblocking command.
        R 0 LPUSH $key value
        wait_for_blocked_clients_count 0

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set lpush_usec [get_cmdstat_usec lpush r]
        set blpop_usec [get_cmdstat_usec blpop r]

        # Assert that both blocking and non-blocking command times have been accumulated.
        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec [expr $lpush_usec + $blpop_usec]
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for blocking commands, unblocked on timeout." {
        # Blocking command with 0.5 seconds timeout.
        set rd [valkey_deferring_client]
        $rd BLPOP $key 0.5

        # Confirm that the client is blocked, then unblocked within 1 second.
        wait_for_blocked_clients_count 1
        wait_for_blocked_clients_count 0

        # Assert that the blocking command time has been accumulated.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set blpop_usec [get_cmdstat_usec blpop r]
        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec $blpop_usec
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for transactions." {
        set r1 [valkey_client]
        $r1 MULTI
        $r1 SET $key value
        $r1 GET $key

        # CPU metric is not accumulated until EXEC is reached. This behaviour is identical to INFO COMMANDSTATS.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert

        # Execute transaction, and assert that all nested command times have been accumulated.
        $r1 EXEC
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set exec_usec [get_cmdstat_usec exec r]
        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec $exec_usec
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for lua-scripts, without cross-slot keys." {
        r eval [format "#!lua
            redis.call('set', '%s', 'bar'); redis.call('get', '%s')" $key $key] 0

        set eval_usec [get_cmdstat_usec eval r]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]

        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec $eval_usec
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for lua-scripts, with cross-slot keys." {
        r eval [format "#!lua flags=allow-cross-slot-keys
            redis.call('set', '%s', 'bar'); redis.call('get', '%s');
        " $key $key_secondary] 0

        # For cross-slot, we do not accumulate at all.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for functions, without cross-slot keys." {
        set function_str [format "#!lua name=f1
            server.register_function{
                function_name='f1',
                callback=function() redis.call('set', '%s', '1') redis.call('get', '%s') end
            }" $key $key]
        r function load replace $function_str
        r fcall f1 0

        set fcall_usec [get_cmdstat_usec fcall r]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]

        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec $fcall_usec
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for functions, with cross-slot keys." {
        set function_str [format "#!lua name=f1
            server.register_function{
                function_name='f1',
                callback=function() redis.call('set', '%s', '1') redis.call('get', '%s') end,
                flags={'allow-cross-slot-keys'}
            }" $key $key_secondary]
        r function load replace $function_str
        r fcall f1 0

        # For cross-slot, we do not accumulate at all.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS network-bytes-in.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {

    # Define shared variables.
    set key "key"
    set key_slot [R 0 cluster keyslot $key]
    set metrics_to_assert [list network-bytes-in]

    test "CLUSTER SLOT-STATS network-bytes-in, multi bulk buffer processing." {
        # *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 33 bytes.
        R 0 SET $key value

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 33
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-in, in-line buffer processing." {
        set rd [valkey_deferring_client]
        # SET key value\r\n --> 15 bytes.
        $rd write "SET $key value\r\n"
        $rd flush
        # Wait for response to ensure command is fully processed.
        $rd read

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 15
            ]
        ]

        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-in, blocking command." {
        set rd [valkey_deferring_client]
        # *3\r\n$5\r\nblpop\r\n$3\r\nkey\r\n$1\r\n0\r\n --> 31 bytes.
        $rd BLPOP $key 0
        wait_for_blocked_clients_count 1

        # Slot-stats must be empty here, as the client is yet to be unblocked.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert

        # *3\r\n$5\r\nlpush\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 35 bytes.
        R 0 LPUSH $key value
        wait_for_blocked_clients_count 0

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 66 ;# 31 + 35 bytes.
            ]
        ]

        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-in, multi-exec transaction." {
        set r [valkey_client]
        # *1\r\n$5\r\nmulti\r\n --> 15 bytes.
        $r MULTI
        # *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 33 bytes.
        assert {[$r SET $key value] eq {QUEUED}}
        # *1\r\n$4\r\nexec\r\n --> 14 bytes.
        assert {[$r EXEC] eq {OK}}

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 62 ;# 15 + 33 + 14 bytes.
            ]
        ]

        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-in, non slot specific command." {
        R 0 INFO

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-in, pub/sub." {
        # PUB/SUB does not get accumulated at per-slot basis, 
        # as it is cluster-wide and is not slot specific.
        set rd [valkey_deferring_client]
        $rd subscribe channel
        R 0 publish channel message

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL
}

start_cluster 1 1 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {
    set channel "channel"
    set key_slot [R 0 cluster keyslot $channel]
    set metrics_to_assert [list network-bytes-in]

    # Setup replication.
    assert {[s -1 role] eq {slave}}
    wait_for_condition 1000 50 {
        [s -1 master_link_status] eq {up}
    } else {
        fail "Instance #1 master link status is not up"
    }
    R 1 readonly

    test "CLUSTER SLOT-STATS network-bytes-in, sharded pub/sub." {
        set slot [R 0 cluster keyslot $channel]
        set primary [Rn 0]
        set replica [Rn 1]
        set replica_subscriber [valkey_deferring_client -1]
        $replica_subscriber SSUBSCRIBE $channel
        # *2\r\n$10\r\nssubscribe\r\n$7\r\nchannel\r\n --> 34 bytes.
        $primary SPUBLISH $channel hello
        # *3\r\n$8\r\nspublish\r\n$7\r\nchannel\r\n$5\r\nhello\r\n --> 42 bytes.

        set slot_stats [$primary CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 42
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert

        set slot_stats [$replica CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 34
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS network-bytes-out correctness.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    # Define shared variables.
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]
    set expected_slots_to_key_count [dict create $key_slot 1]
    set metrics_to_assert [list network-bytes-out]
    R 0 CONFIG SET cluster-slot-stats-enabled yes

    test "CLUSTER SLOT-STATS network-bytes-out, for non-slot specific commands." {
        R 0 INFO
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-out, for slot specific commands." {
        R 0 SET $key value
        # +OK\r\n --> 5 bytes

        R 0 GET $key
        # $5\r\nvalue\r\n -> 11 bytes

        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 16
            ]
        ]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-out, for slot specific commands, for reply copy avoidance" {
        set copy_avoid [lindex [R 0 config get min-string-size-avoid-copy-reply] 1]
        R 0 config set min-string-size-avoid-copy-reply 1

        set value [string repeat A 1024] ;# Make sure it is a RAW string
        R 0 set $key $value ;# +OK\r\n --> 5 bytes
        R 0 get $key        ;# $1024\r\nAA..AA\r\n -> 1033 bytes

        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 1038
            ]
        ]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE $key_slot $key_slot]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert

        R 0 config set min-string-size-avoid-copy-reply $copy_avoid
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-out, blocking commands." {
        set rd [valkey_deferring_client]
        $rd BLPOP $key 0
        wait_for_blocked_clients_count 1

        # Assert empty slot stats here, since COB is yet to be flushed due to the block.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert

        # Unblock the command.
        # LPUSH client) :1\r\n --> 4 bytes.
        # BLPOP client) *2\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 24 bytes, upon unblocking.
        R 0 LPUSH $key value
        wait_for_blocked_clients_count 0

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 28 ;# 4 + 24 bytes.
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL
}

start_cluster 1 1 {tags {external:skip cluster}} {

    # Define shared variables.
    set key "FOO"
    set key_slot [R 0 CLUSTER KEYSLOT $key]
    set metrics_to_assert [list network-bytes-out]
    R 0 CONFIG SET cluster-slot-stats-enabled yes

    # Setup replication.
    assert {[s -1 role] eq {slave}}
    wait_for_condition 1000 50 {
        [s -1 master_link_status] eq {up}
    } else {
        fail "Instance #1 master link status is not up"
    }
    R 1 readonly

    test "CLUSTER SLOT-STATS network-bytes-out, replication stream egress." {
        assert_equal [R 0 SET $key VALUE] {OK}
        # Local client) +OK\r\n --> 5 bytes.
        # Replication stream) *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 33 bytes.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 38 ;# 5 + 33 bytes.
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
}

start_cluster 1 1 {tags {external:skip cluster}} {

    # Define shared variables.
    set channel "channel"
    set key_slot [R 0 cluster keyslot $channel]
    set channel_secondary "channel2"
    set key_slot_secondary [R 0 cluster keyslot $channel_secondary]
    set metrics_to_assert [list network-bytes-out]
    R 0 CONFIG SET cluster-slot-stats-enabled yes

    test "CLUSTER SLOT-STATS network-bytes-out, sharded pub/sub, single channel." {
        set slot [R 0 cluster keyslot $channel]
        set publisher [Rn 0]
        set subscriber [valkey_client]
        set replica [valkey_deferring_client -1]

        # Subscriber client) *3\r\n$10\r\nssubscribe\r\n$7\r\nchannel\r\n:1\r\n --> 38 bytes
        $subscriber SSUBSCRIBE $channel 
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 38
            ]
        ]
        R 0 CONFIG RESETSTAT

        # Publisher client) :1\r\n --> 4 bytes.
        # Subscriber client) *3\r\n$8\r\nsmessage\r\n$7\r\nchannel\r\n$5\r\nhello\r\n --> 42 bytes.
        assert_equal 1 [$publisher SPUBLISH $channel hello]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 46 ;# 4 + 42 bytes.
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    $subscriber QUIT
    R 0 FLUSHALL
    R 0 CONFIG RESETSTAT

    test "CLUSTER SLOT-STATS network-bytes-out, sharded pub/sub, cross-slot channels." {
        set slot [R 0 cluster keyslot $channel]
        set publisher [Rn 0]
        set subscriber [valkey_client]
        set replica [valkey_deferring_client -1]

        # Stack multi-slot subscriptions against a single client.
        # For primary channel;
        # Subscriber client) *3\r\n$10\r\nssubscribe\r\n$7\r\nchannel\r\n:1\r\n --> 38 bytes
        # For secondary channel;
        # Subscriber client) *3\r\n$10\r\nssubscribe\r\n$8\r\nchannel2\r\n:1\r\n --> 39 bytes
        $subscriber SSUBSCRIBE $channel
        $subscriber SSUBSCRIBE $channel_secondary
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create \
                $key_slot [ \
                    dict create network-bytes-out 38
                ] \
                $key_slot_secondary [ \
                    dict create network-bytes-out 39
                ]
        ]
        R 0 CONFIG RESETSTAT

        # For primary channel;
        # Publisher client) :1\r\n --> 4 bytes.
        # Subscriber client) *3\r\n$8\r\nsmessage\r\n$7\r\nchannel\r\n$5\r\nhello\r\n --> 42 bytes.
        # For secondary channel;
        # Publisher client) :1\r\n --> 4 bytes.
        # Subscriber client) *3\r\n$8\r\nsmessage\r\n$8\r\nchannel2\r\n$5\r\nhello\r\n --> 43 bytes.
        assert_equal 1 [$publisher SPUBLISH $channel hello]
        assert_equal 1 [$publisher SPUBLISH $channel_secondary hello]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create \
                $key_slot [ \
                    dict create network-bytes-out 46 ;# 4 + 42 bytes.
                ] \
                $key_slot_secondary [ \
                    dict create network-bytes-out 47 ;# 4 + 43 bytes.
                ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS key-count metric correctness.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {

    # Define shared variables.
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]
    set metrics_to_assert [list key-count]
    set expected_slot_stats [
        dict create $key_slot [
            dict create key-count 1
        ]
    ]

    test "CLUSTER SLOT-STATS contains default value upon valkey-server startup" {
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }

    test "CLUSTER SLOT-STATS contains correct metrics upon key introduction" {
        R 0 SET $key TEST
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }

    test "CLUSTER SLOT-STATS contains correct metrics upon key mutation" {
        R 0 SET $key NEW_VALUE
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }

    test "CLUSTER SLOT-STATS contains correct metrics upon key deletion" {
        R 0 DEL $key
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }

    test "CLUSTER SLOT-STATS slot visibility based on slot ownership changes" {
        R 0 CONFIG SET cluster-require-full-coverage no

        R 0 CLUSTER DELSLOTS $key_slot
        set expected_slots [initialize_expected_slots_dict]
        dict unset expected_slots $key_slot
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert {[dict size $expected_slots] == 16383}
        assert_slot_visibility $slot_stats $expected_slots

        R 0 CLUSTER ADDSLOTS $key_slot
        set expected_slots [initialize_expected_slots_dict]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert {[dict size $expected_slots] == 16384}
        assert_slot_visibility $slot_stats $expected_slots
    }
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS SLOTSRANGE sub-argument.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {

    test "CLUSTER SLOT-STATS SLOTSRANGE all slots present" {
        set start_slot 100
        set end_slot 102
        set expected_slots [initialize_expected_slots_dict_with_range $start_slot $end_slot]

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE $start_slot $end_slot]
        assert_slot_visibility $slot_stats $expected_slots
    }

    test "CLUSTER SLOT-STATS SLOTSRANGE some slots missing" {
        set start_slot 100
        set end_slot 102
        set expected_slots [initialize_expected_slots_dict_with_range $start_slot $end_slot]

        R 0 CLUSTER DELSLOTS $start_slot
        dict unset expected_slots $start_slot

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE $start_slot $end_slot]
        assert_slot_visibility $slot_stats $expected_slots
    }
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS ORDERBY sub-argument.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {

    set metrics [list "key-count" "cpu-usec" "network-bytes-in" "network-bytes-out"]

    # SET keys for target hashslots, to encourage ordering.
    set hash_tags [list 0 1 2 3 4]
    set num_keys 1
    foreach hash_tag $hash_tags {
        for {set i 0} {$i < $num_keys} {incr i 1} {
            R 0 SET "$i{$hash_tag}" VALUE
        }
        incr num_keys 1
    }

    # SET keys for random hashslots, for random noise.
    set num_keys 0
    while {$num_keys < 1000} {
        set random_key [randomInt 16384]
        R 0 SET $random_key VALUE
        incr num_keys 1
    }

    test "CLUSTER SLOT-STATS ORDERBY DESC correct ordering" {
        foreach orderby $metrics {
            set slot_stats [R 0 CLUSTER SLOT-STATS ORDERBY $orderby DESC]
            assert_slot_stats_monotonic_descent $slot_stats $orderby
        }
    }

    test "CLUSTER SLOT-STATS ORDERBY ASC correct ordering" {
        foreach orderby $metrics {
            set slot_stats [R 0 CLUSTER SLOT-STATS ORDERBY $orderby ASC]
            assert_slot_stats_monotonic_ascent $slot_stats $orderby
        }
    }

    test "CLUSTER SLOT-STATS ORDERBY LIMIT correct response pagination, where limit is less than number of assigned slots" {
        R 0 FLUSHALL SYNC
        R 0 CONFIG RESETSTAT

        foreach orderby $metrics {
            set limit 5
            set slot_stats_desc [R 0 CLUSTER SLOT-STATS ORDERBY $orderby LIMIT $limit DESC]
            set slot_stats_asc [R 0 CLUSTER SLOT-STATS ORDERBY $orderby LIMIT $limit ASC]
            set slot_stats_desc_length [llength $slot_stats_desc]
            set slot_stats_asc_length [llength $slot_stats_asc]
            assert {$limit == $slot_stats_desc_length && $limit == $slot_stats_asc_length}

            # All slot statistics have been reset to 0, so we will order by slot in ascending order.
            set expected_slots [dict create 0 0 1 0 2 0 3 0 4 0]
            assert_slot_visibility $slot_stats_desc $expected_slots
            assert_slot_visibility $slot_stats_asc $expected_slots
        }
    }

    test "CLUSTER SLOT-STATS ORDERBY LIMIT correct response pagination, where limit is greater than number of assigned slots" {
        R 0 CONFIG SET cluster-require-full-coverage no
        R 0 FLUSHALL SYNC
        R 0 CLUSTER FLUSHSLOTS
        R 0 CLUSTER ADDSLOTS 100 101

        foreach orderby $metrics {
            set num_assigned_slots 2
            set limit 5
            set slot_stats_desc [R 0 CLUSTER SLOT-STATS ORDERBY $orderby LIMIT $limit DESC]
            set slot_stats_asc [R 0 CLUSTER SLOT-STATS ORDERBY $orderby LIMIT $limit ASC]
            set slot_stats_desc_length [llength $slot_stats_desc]
            set slot_stats_asc_length [llength $slot_stats_asc]
            set expected_response_length [expr min($num_assigned_slots, $limit)]
            assert {$expected_response_length == $slot_stats_desc_length && $expected_response_length == $slot_stats_asc_length}

            set expected_slots [dict create 100 0 101 0]
            assert_slot_visibility $slot_stats_desc $expected_slots
            assert_slot_visibility $slot_stats_asc $expected_slots
        }
    }

    test "CLUSTER SLOT-STATS ORDERBY arg sanity check." {
        # Non-existent argument.
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY key-count non-existent-arg}
        # Negative LIMIT.
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY key-count DESC LIMIT -1}
        # Non-existent ORDERBY metric.
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY non-existent-metric}
        # When cluster-slot-stats-enabled config is disabled, you cannot sort using advanced metrics.
        R 0 CONFIG SET cluster-slot-stats-enabled no
        set orderby "cpu-usec"
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY $orderby}
        set orderby "network-bytes-in"
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY $orderby}
        set orderby "network-bytes-out"
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY $orderby}
    }

}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS replication.
# -----------------------------------------------------------------------------

start_cluster 1 1 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {

    # Define shared variables.
    set key "key"
    set key_slot [R 0 CLUSTER KEYSLOT $key]
    set primary [Rn 0]
    set replica [Rn 1]

    # For replication, assertions are split between deterministic and non-deterministic metrics.
    # * For deterministic metrics, strict equality assertions are made.
    # * For non-deterministic metrics, non-zeroness assertions are made. 
    #   Non-zeroness as in, both primary and replica should either have some value, or no value at all.
    #
    # * key-count is deterministic between primary and its replica.
    # * cpu-usec is non-deterministic between primary and its replica.
    # * network-bytes-in is deterministic between primary and its replica.
    # * network-bytes-out will remain empty in the replica, since primary client do not receive replies, unless for replicationSendAck().
    set deterministic_metrics [list key-count network-bytes-in]
    set non_deterministic_metrics [list cpu-usec]
    set empty_metrics [list network-bytes-out]

    # Setup replication.
    assert {[s -1 role] eq {slave}}
    wait_for_condition 1000 50 {
        [s -1 master_link_status] eq {up}
    } else {
        fail "Instance #1 master link status is not up"
    }
    R 1 readonly

    test "CLUSTER SLOT-STATS metrics replication for new keys" {
        # *3\r\n$3\r\nset\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 33 bytes.
        R 0 SET $key VALUE

        set expected_slot_stats [
            dict create $key_slot [
                dict create key-count 1 network-bytes-in 33
            ]
        ]
        set slot_stats_master [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats_master $expected_slot_stats $deterministic_metrics

        wait_for_condition 500 10 {
            [string match {*calls=1,*} [cmdrstat set $replica]]
        } else {
            fail "Replica did not receive the command."
        }
        set slot_stats_replica [R 1 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_equal_slot_stats $slot_stats_master $slot_stats_replica $deterministic_metrics $non_deterministic_metrics
        assert_empty_slot_stats $slot_stats_replica $empty_metrics
    }
    R 0 CONFIG RESETSTAT
    R 1 CONFIG RESETSTAT

    test "CLUSTER SLOT-STATS metrics replication for existing keys" {
        # *3\r\n$3\r\nset\r\n$3\r\nkey\r\n$13\r\nvalue_updated\r\n --> 42 bytes.
        R 0 SET $key VALUE_UPDATED

        set expected_slot_stats [
            dict create $key_slot [
                dict create key-count 1 network-bytes-in 42
            ]
        ]
        set slot_stats_master [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats_master $expected_slot_stats $deterministic_metrics

        wait_for_condition 500 10 {
            [string match {*calls=1,*} [cmdrstat set $replica]]
        } else {
            fail "Replica did not receive the command."
        }
        set slot_stats_replica [R 1 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_equal_slot_stats $slot_stats_master $slot_stats_replica $deterministic_metrics $non_deterministic_metrics
        assert_empty_slot_stats $slot_stats_replica $empty_metrics
    }
    R 0 CONFIG RESETSTAT
    R 1 CONFIG RESETSTAT

    test "CLUSTER SLOT-STATS metrics replication for deleting keys" {
        # *2\r\n$3\r\ndel\r\n$3\r\nkey\r\n --> 22 bytes.
        R 0 DEL $key

        set expected_slot_stats [
            dict create $key_slot [
                dict create key-count 0 network-bytes-in 22
            ]
        ]
        set slot_stats_master [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats_master $expected_slot_stats $deterministic_metrics

        wait_for_condition 500 10 {
            [string match {*calls=1,*} [cmdrstat del $replica]]
        } else {
            fail "Replica did not receive the command."
        }
        set slot_stats_replica [R 1 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_equal_slot_stats $slot_stats_master $slot_stats_replica $deterministic_metrics $non_deterministic_metrics
        assert_empty_slot_stats $slot_stats_replica $empty_metrics
    }
    R 0 CONFIG RESETSTAT
    R 1 CONFIG RESETSTAT
}

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {
    set key "testslotbytes"
    set key_slot [R 0 cluster keyslot $key]
    set metrics_to_assert [list network-bytes-out]

    test "CLUSTER SLOT-STATS network-bytes-out with copy avoidance and commandlog disabled" {
        set copy_avoid [lindex [R 0 config get min-string-size-avoid-copy-reply] 1]
        R 0 config set min-string-size-avoid-copy-reply 1
        
        # Disable commandlog tracking
        R 0 config set commandlog-reply-larger-than -1
        R 0 config resetstat
        
        set value [string repeat A 2048]
        R 0 set $key $value
        
        # Reset stats after SET to only measure GET reply
        R 0 config resetstat
        
        # Get should use copy avoidance path
        R 0 get $key
        
        # Verify cluster slot stats tracked the bytes correctly
        # Even though commandlog tracking is disabled, cluster slot stats should work
        # via IO thread accounting in releaseBufReferences()
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set slot_stats [convert_array_into_dict $slot_stats]
        
        set network_bytes_out [dict get $slot_stats $key_slot network-bytes-out]
        
        # For 2048 bytes: $2048\r\n<data>\r\n = 2057 bytes
        assert_equal $network_bytes_out 2057
        
        # Re-enable commandlog
        R 0 config set commandlog-reply-larger-than 1024
        R 0 config resetstat
        
        # Get should still track correctly
        R 0 get $key
        
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set slot_stats [convert_array_into_dict $slot_stats]
        set network_bytes_out [dict get $slot_stats $key_slot network-bytes-out]
        
        assert_equal $network_bytes_out 2057
        
        # Cleanup
        R 0 del $key
        R 0 config set min-string-size-avoid-copy-reply $copy_avoid
    }
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS memory-data-bytes / memory-overhead-bytes.
# Uses DEBUG SLOT-VERIFY-MEMORY to independently walk all keys in a slot and
# verify that slot_stats match. The walk does NOT use objectLogicalSize or any
# tracking field — only pre-existing APIs.
# -----------------------------------------------------------------------------

# Helper: get memory-data-bytes and memory-overhead-bytes for a given slot.
proc get_slot_memory {slot} {
    set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE $slot $slot]
    set slot_stats [convert_array_into_dict $slot_stats]
    set stats [dict get $slot_stats $slot]
    set data [dict get $stats memory-data-bytes]
    set overhead [dict get $stats memory-overhead-bytes]
    return [list $data $overhead]
}

# Helper: assert slot memory matches independent walk via DEBUG command.
proc verify_slot_memory {slot} {
    set result [R 0 DEBUG SLOT-VERIFY-MEMORY $slot]
    if {$result ne "OK"} {
        fail "SLOT-VERIFY-MEMORY slot $slot: $result"
    }
}

start_cluster 1 0 {tags {external:skip cluster needs:debug} overrides {cluster-slot-stats-enabled yes enable-debug-command yes}} {

    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]
    set key2 "BAR"
    set key2_slot [R 0 cluster keyslot $key2]

    test "SLOT-STATS memory, initially zero." {
        lassign [get_slot_memory $key_slot] data overhead
        assert_equal $data 0
        assert_equal $overhead 0
    }

    test "SLOT-STATS memory, string SET and verify." {
        R 0 SET $key "hello world"
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data overhead
        assert {$data > 0}
        assert_equal $overhead 0
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, string overwrite changes size." {
        R 0 SET $key "small"
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_small _

        R 0 SET $key [string repeat "x" 1000]
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_large _
        assert {$data_large > $data_small}
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, integer string has zero data." {
        R 0 SET $key 42
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data overhead
        assert_equal $data 0
        assert_equal $overhead 0
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, DEL removes all memory." {
        R 0 SET $key [string repeat "x" 500]
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_before _
        assert {$data_before > 0}

        R 0 DEL $key
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_after _
        assert_equal $data_after 0
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, two keys in same slot." {
        set tag_key1 "{$key}:a"
        set tag_key2 "{$key}:b"
        R 0 SET $tag_key1 "aaa"
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data1 _

        R 0 SET $tag_key2 "bbbbb"
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data2 _
        assert {$data2 > $data1}
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, different slots are independent." {
        R 0 SET $key "aaaa"
        R 0 SET $key2 [string repeat "b" 100]
        verify_slot_memory $key_slot
        verify_slot_memory $key2_slot

        R 0 DEL $key
        verify_slot_memory $key_slot
        verify_slot_memory $key2_slot
        lassign [get_slot_memory $key_slot] data1 _
        lassign [get_slot_memory $key2_slot] data2 _
        assert_equal $data1 0
        assert {$data2 > 0}
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, FLUSHALL resets to zero." {
        R 0 SET $key "value1"
        R 0 SET $key2 "value2"
        R 0 FLUSHALL
        lassign [get_slot_memory $key_slot] data1 _
        lassign [get_slot_memory $key2_slot] data2 _
        assert_equal $data1 0
        assert_equal $data2 0
    }

    test "SLOT-STATS memory, hash in-place growth." {
        # Force hashtable encoding with enough fields.
        R 0 CONFIG SET hash-max-listpack-entries 0
        for {set i 0} {$i < 50} {incr i} {
            R 0 HSET $key "field_$i" "value_$i"
            verify_slot_memory $key_slot
        }
        lassign [get_slot_memory $key_slot] data overhead
        assert {$data > 0}
        assert {$overhead > 0} ;# hashtable encoding has bucket overhead
        R 0 CONFIG SET hash-max-listpack-entries 128
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, set add and remove." {
        for {set i 0} {$i < 200} {incr i} {
            R 0 SADD $key "member_$i"
        }
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_full _

        for {set i 0} {$i < 100} {incr i} {
            R 0 SREM $key "member_$i"
        }
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_half _
        assert {$data_half < $data_full}
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, list push and pop." {
        for {set i 0} {$i < 100} {incr i} {
            R 0 RPUSH $key "item_$i"
        }
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_full overhead_full

        for {set i 0} {$i < 50} {incr i} {
            R 0 LPOP $key
        }
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_half _
        assert {$data_half < $data_full}
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, stream append and trim." {
        # Use small node size so entries span multiple rax nodes.
        # XTRIM only frees entire rax nodes — tombstoned entries within
        # a listpack don't reduce lpBytes.
        R 0 CONFIG SET stream-node-max-entries 5
        for {set i 0} {$i < 50} {incr i} {
            R 0 XADD $key "*" field_$i value_$i
        }
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_before _
        assert {$data_before > 0}

        R 0 XTRIM $key MAXLEN 5
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_after _
        assert {$data_after < $data_before}
        R 0 CONFIG SET stream-node-max-entries 100
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, mixed types in same slot." {
        set str_key "{$key}:str"
        set hash_key "{$key}:hash"
        set set_key "{$key}:set"

        R 0 SET $str_key "hello"
        verify_slot_memory $key_slot

        for {set i 0} {$i < 200} {incr i} {
            R 0 HSET $hash_key "f_$i" "v_$i"
        }
        verify_slot_memory $key_slot

        for {set i 0} {$i < 200} {incr i} {
            R 0 SADD $set_key "m_$i"
        }
        verify_slot_memory $key_slot
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, DEBUG SLOT-VERIFY-MEMORY detects mismatch." {
        # Create a key so slot has non-zero memory.
        R 0 SET $key "some data here"
        verify_slot_memory $key_slot

        # CONFIG RESETSTAT zeros slot_stats but leaves keys in place,
        # creating an intentional mismatch.
        R 0 CONFIG RESETSTAT

        # Verification must now FAIL because stats say 0 but key exists.
        catch {R 0 DEBUG SLOT-VERIFY-MEMORY $key_slot} err
        assert_match "*mismatch*" $err

        # Restore correct state: FLUSHALL resets counters and removes keys.
        R 0 FLUSHALL
    }

    test "SLOT-STATS memory, MULTI/EXEC tracks all sub-commands." {
        # Force hashtable encoding so we get overhead from bucket arrays.
        R 0 CONFIG SET hash-max-listpack-entries 0
        R 0 CONFIG SET set-max-listpack-entries 0

        set r [valkey_client]
        $r MULTI
        $r SET $key [string repeat "a" 100]
        for {set i 0} {$i < 50} {incr i} {
            $r HSET "{$key}:hash" "field_$i" "value_$i"
        }
        for {set i 0} {$i < 50} {incr i} {
            $r SADD "{$key}:set" "member_$i"
        }
        for {set i 0} {$i < 50} {incr i} {
            $r RPUSH "{$key}:list" "item_$i"
        }
        $r EXEC

        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data overhead
        assert {$data > 0}
        assert {$overhead > 0}

        R 0 CONFIG SET hash-max-listpack-entries 128
        R 0 CONFIG SET set-max-listpack-entries 128
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, ORDERBY memory-data-bytes." {
        R 0 SET $key "small"
        R 0 SET $key2 [string repeat "x" 500]
        set slot_stats [R 0 CLUSTER SLOT-STATS ORDERBY memory-data-bytes LIMIT 2 DESC]
        assert_slot_stats_monotonic_descent $slot_stats memory-data-bytes
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, write to expired key (lazy expiry + write, no double-count)." {
        # Disable active expiry so the key is NOT cleaned up in the background.
        # This forces the next write command to trigger lazy expiry inline.
        R 0 DEBUG SET-ACTIVE-EXPIRE 0

        R 0 SET $key [string repeat "a" 200] PX 100
        verify_slot_memory $key_slot
        after 200

        # Key is expired but still in DB (active expiry disabled).
        # This SET triggers lazy expiry DURING the command. The explicit
        # expiry hook must NOT fire (call() hooks handle it), otherwise
        # the old size is subtracted twice.
        R 0 SET $key [string repeat "b" 300]
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data _
        assert {$data > 0}

        R 0 DEBUG SET-ACTIVE-EXPIRE 1
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, key expiry removes memory." {
        # Set a key with 1 second TTL.
        R 0 SET $key [string repeat "x" 200] PX 500
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_before _
        assert {$data_before > 0}

        # Wait for the key to expire.
        after 1000
        # Access the slot to trigger lazy expiry or wait for active expiry.
        R 0 GET $key

        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_after _
        assert_equal $data_after 0
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, hash rehash overhead repro." {
        R 0 CONFIG SET hash-max-listpack-entries 0
        # Add fields until resize triggers, then verify after each.
        for {set i 0} {$i < 100} {incr i} {
            R 0 HSET $key "field_$i" [string repeat "v" 20]
            lassign [get_slot_memory $key_slot] d o
            if {[catch {R 0 DEBUG SLOT-VERIFY-MEMORY $key_slot} err]} {
                puts "FAIL at HSET field_$i: data=$d overhead=$o err=$err"
                fail "hash rehash repro: $err"
            }
        }
        # Now delete fields one by one and verify.
        for {set i 0} {$i < 50} {incr i} {
            R 0 HDEL $key "field_$i"
            lassign [get_slot_memory $key_slot] d o
            if {[catch {R 0 DEBUG SLOT-VERIFY-MEMORY $key_slot} err]} {
                puts "FAIL at HDEL field_$i: data=$d overhead=$o err=$err"
                fail "hash rehash repro: $err"
            }
        }
        # Add more to trigger another resize cycle.
        for {set i 100} {$i < 200} {incr i} {
            R 0 HSET $key "field_$i" [string repeat "w" 20]
            lassign [get_slot_memory $key_slot] d o
            if {[catch {R 0 DEBUG SLOT-VERIFY-MEMORY $key_slot} err]} {
                puts "FAIL at HSET field_$i (2nd wave): data=$d overhead=$o err=$err"
                fail "hash rehash repro: $err"
            }
        }
        R 0 CONFIG SET hash-max-listpack-entries 128
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, hash+set rehash overhead repro." {
        R 0 CONFIG SET hash-max-listpack-entries 0
        set hash_key "{$key}:hash"
        set set_key "{$key}:set"

        for {set round 0} {$round < 300} {incr round} {
            R 0 FLUSHALL

            # Grow hash and set to trigger resize on both.
            for {set i 0} {$i < 30} {incr i} {
                R 0 HSET $hash_key "f_$i" [string repeat "v" 20]
                R 0 SADD $set_key "m_$i"
            }

            # Interleave writes AND reads — reads (HGET, SISMEMBER) trigger
            # rehash steps on the value HT without signalModifiedKey.
            for {set i 0} {$i < 50} {incr i} {
                set op [expr {int(rand() * 6)}]
                switch $op {
                    0 { R 0 HSET $hash_key "f_[expr {int(rand() * 50)}]" [string repeat "x" 20] }
                    1 { catch { R 0 HDEL $hash_key "f_[expr {int(rand() * 50)}]" } }
                    2 { R 0 SADD $set_key "m_[expr {int(rand() * 200)}]" }
                    3 { catch { R 0 SREM $set_key "m_[expr {int(rand() * 200)}]" } }
                    4 { catch { R 0 HGET $hash_key "f_[expr {int(rand() * 50)}]" } }
                    5 { catch { R 0 SISMEMBER $set_key "m_[expr {int(rand() * 200)}]" } }
                }

                if {[catch {R 0 DEBUG SLOT-VERIFY-MEMORY $key_slot} err]} {
                    lassign [get_slot_memory $key_slot] d o
                    puts "FAIL round=$round iter=$i op=$op data=$d overhead=$o err=$err"
                    fail "hash+set rehash repro: $err"
                }
            }
        }
        R 0 CONFIG SET hash-max-listpack-entries 128
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, fuzzer: random operations across types and slots." {
        R 0 CONFIG SET hash-max-listpack-entries 0

        set slots [list $key_slot $key2_slot]
        set prefixes [list "{$key}" "{$key2}"]
        set types {string hash set list}
        set counter 0

        for {set iter 0} {$iter < 200} {incr iter} {
            # Pick a random slot and type.
            set sidx [expr {int(rand() * 2)}]
            set prefix [lindex $prefixes $sidx]
            set slot [lindex $slots $sidx]
            set type [lindex $types [expr {int(rand() * [llength $types])}]]
            set k "$prefix:fuzz_${type}"

            # Pick a random operation.
            set op [expr {int(rand() * 5)}]

            switch $type {
                string {
                    switch $op {
                        0 - 1 { R 0 SET $k [string repeat "v" [expr {int(rand() * 500) + 1}]] }
                        2     { R 0 APPEND $k "extra" }
                        3     { R 0 SET $k [incr counter] }
                        4     { catch { R 0 DEL $k } }
                    }
                }
                hash {
                    set f "f_[expr {int(rand() * 50)}]"
                    switch $op {
                        0 - 1 { R 0 HSET $k $f [string repeat "h" [expr {int(rand() * 100) + 1}]] }
                        2     { catch { R 0 HDEL $k $f } }
                        3     { R 0 HSET $k "new_$iter" "val_$iter" }
                        4     { catch { R 0 DEL $k } }
                    }
                }
                set {
                    switch $op {
                        0 - 1 { R 0 SADD $k "m_[expr {int(rand() * 200)}]" }
                        2     { catch { R 0 SREM $k "m_[expr {int(rand() * 200)}]" } }
                        3     { catch { R 0 SPOP $k } }
                        4     { catch { R 0 DEL $k } }
                    }
                }
                list {
                    switch $op {
                        0 - 1 { R 0 RPUSH $k "item_[expr {int(rand() * 100)}]" }
                        2     { catch { R 0 LPOP $k } }
                        3     { R 0 RPUSH $k [string repeat "l" [expr {int(rand() * 200) + 1}]] }
                        4     { catch { R 0 DEL $k } }
                    }
                }
            }

            # Verify every 10 operations.
            if {$iter % 10 == 9} {
                verify_slot_memory $key_slot
                verify_slot_memory $key2_slot
            }
        }

        # Final verification.
        verify_slot_memory $key_slot
        verify_slot_memory $key2_slot

        R 0 CONFIG SET hash-max-listpack-entries 128
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, eviction removes memory." {
        # Fill the slot with keys, then set a tight maxmemory to trigger eviction.
        for {set i 0} {$i < 20} {incr i} {
            R 0 SET "{$key}:evict_$i" [string repeat "x" 500]
        }
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_before _
        assert {$data_before > 0}

        # Set maxmemory to trigger eviction of some keys.
        set used [s 0 used_memory]
        R 0 CONFIG SET maxmemory-policy allkeys-lru
        R 0 CONFIG SET maxmemory [expr {$used - 2000}]

        # Force eviction by trying to add data.
        catch { R 0 SET "{$key}:trigger" [string repeat "y" 500] }

        verify_slot_memory $key_slot

        # Restore.
        R 0 CONFIG SET maxmemory 0
        R 0 CONFIG SET maxmemory-policy noeviction
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, hash field expiry reduces memory." {
        # Create a hash with fields that have short TTL.
        R 0 CONFIG SET hash-max-listpack-entries 0
        R 0 HSETEX $key EX 1 FIELDS 3 f1 v1 f2 v2 f3 v3
        # Add some non-expiring fields too.
        R 0 HSET $key persistent1 value1 persistent2 value2

        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_before overhead_before
        assert {$data_before > 0}

        # Wait for fields to expire.
        after 2000

        # Trigger active expiry by accessing any key — the server's
        # activeExpireCycle will process expired hash fields.
        R 0 PING

        # The hash should still exist (persistent fields remain) but be smaller.
        assert {[R 0 HLEN $key] == 2}
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_after _
        assert {$data_after < $data_before}

        R 0 CONFIG SET hash-max-listpack-entries 128
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, hash field expiry deletes empty key." {
        # Create a hash where ALL fields expire.
        R 0 CONFIG SET hash-max-listpack-entries 0
        R 0 HSETEX $key EX 1 FIELDS 3 f1 v1 f2 v2 f3 v3

        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_before _
        assert {$data_before > 0}

        # Wait for all fields to expire.
        after 2000
        R 0 PING

        # Key should be gone.
        assert {[R 0 EXISTS $key] == 0}
        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_after _
        assert_equal $data_after 0
        R 0 CONFIG SET hash-max-listpack-entries 128
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, DEBUG RELOAD preserves correctness." {
        # Create keys of various types.
        R 0 SET "{$key}:str" [string repeat "a" 100]
        R 0 CONFIG SET hash-max-listpack-entries 0
        for {set i 0} {$i < 30} {incr i} {
            R 0 HSET "{$key}:hash" "f_$i" "v_$i"
        }
        for {set i 0} {$i < 200} {incr i} {
            R 0 SADD "{$key}:set" "m_$i"
        }
        R 0 CONFIG SET hash-max-listpack-entries 128

        verify_slot_memory $key_slot
        lassign [get_slot_memory $key_slot] data_before overhead_before
        assert {$data_before > 0}

        # Reload from RDB — slot stats are rebuilt via dbAddRDBLoad hook.
        R 0 DEBUG RELOAD

        verify_slot_memory $key_slot
    }
    R 0 FLUSHALL

    test "SLOT-STATS memory, AOF reload preserves correctness." {
        # Enable AOF and wait for any automatic rewrite to finish.
        R 0 CONFIG SET appendonly yes
        R 0 CONFIG SET aof-use-rdb-preamble yes
        wait_for_condition 50 100 {
            [s 0 aof_rewrite_in_progress] == 0
        } else {
            fail "AOF rewrite did not finish"
        }

        # Keys in the RDB preamble.
        R 0 SET "{$key}:aof1" [string repeat "r" 100]
        R 0 SET "{$key}:aof2" [string repeat "s" 200]
        R 0 BGREWRITEAOF
        wait_for_condition 50 100 {
            [s 0 aof_rewrite_in_progress] == 0
        } else {
            fail "AOF rewrite did not finish"
        }

        # Keys in the RESP tail (written after the rewrite).
        R 0 SET "{$key}:aof3" [string repeat "t" 300]
        R 0 SET "{$key2}:aof4" [string repeat "u" 400]

        # Reload from AOF (RDB preamble + RESP tail).
        R 0 DEBUG LOADAOF

        verify_slot_memory $key_slot
        verify_slot_memory $key2_slot
        lassign [get_slot_memory $key_slot] data1 _
        lassign [get_slot_memory $key2_slot] data2 _
        assert {$data1 > 0}
        assert {$data2 > 0}

        R 0 CONFIG SET appendonly no
    }
    R 0 FLUSHALL
}
