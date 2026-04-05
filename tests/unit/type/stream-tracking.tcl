# Stream memory tracking integration tests.
# Uses DEBUG STREAM-VERIFY-TRACKING to validate that tracked_data_bytes
# and tracked_metadata_bytes match a full O(n) walk after each operation.

proc verify_stream_tracking {key} {
    set result [r debug stream-verify-tracking $key]
    assert_equal $result "OK"
}

start_server {tags {"stream needs:debug"}} {
    test {XADD tracking} {
        r DEL mystream
        for {set i 0} {$i < 100} {incr i} {
            r XADD mystream "*" field "value_$i"
        }
        verify_stream_tracking mystream
    }

    test {XADD with multiple fields tracking} {
        r DEL mystream
        for {set i 0} {$i < 50} {incr i} {
            r XADD mystream "*" name "user_$i" age $i email "user_$i@test.com"
        }
        verify_stream_tracking mystream
    }

    test {XTRIM MAXLEN tracking} {
        r DEL mystream
        for {set i 0} {$i < 200} {incr i} {
            r XADD mystream "*" f "v_$i"
        }
        verify_stream_tracking mystream
        r XTRIM mystream MAXLEN 10
        verify_stream_tracking mystream
    }

    test {XTRIM MINID tracking} {
        r DEL mystream
        set ids {}
        for {set i 0} {$i < 100} {incr i} {
            lappend ids [r XADD mystream "*" f v]
        }
        verify_stream_tracking mystream
        # Trim by the 50th ID
        r XTRIM mystream MINID [lindex $ids 50]
        verify_stream_tracking mystream
    }

    test {XDEL tracking} {
        r DEL mystream
        set ids {}
        for {set i 0} {$i < 20} {incr i} {
            lappend ids [r XADD mystream "*" f "v_$i"]
        }
        verify_stream_tracking mystream
        # Delete every other entry
        for {set i 0} {$i < 20} {incr i 2} {
            r XDEL mystream [lindex $ids $i]
        }
        verify_stream_tracking mystream
        # Delete all remaining
        for {set i 1} {$i < 20} {incr i 2} {
            r XDEL mystream [lindex $ids $i]
        }
        verify_stream_tracking mystream
    }

    test {XGROUP CREATE tracking} {
        r DEL mystream
        r XADD mystream "*" f v
        r XGROUP CREATE mystream grp1 0
        verify_stream_tracking mystream
        r XGROUP CREATE mystream grp2 0
        verify_stream_tracking mystream
    }

    test {XGROUP DESTROY tracking} {
        r DEL mystream
        r XADD mystream "*" f v
        r XGROUP CREATE mystream grp1 0
        r XGROUP CREATE mystream grp2 0
        verify_stream_tracking mystream
        r XGROUP DESTROY mystream grp1
        verify_stream_tracking mystream
        r XGROUP DESTROY mystream grp2
        verify_stream_tracking mystream
    }

    test {XREADGROUP creates consumer and NACKs - tracking} {
        r DEL mystream
        for {set i 0} {$i < 10} {incr i} {
            r XADD mystream "*" f "v_$i"
        }
        r XGROUP CREATE mystream grp 0
        # Read creates consumer + NACKs
        r XREADGROUP GROUP grp consumer1 COUNT 5 STREAMS mystream ">"
        verify_stream_tracking mystream
        r XREADGROUP GROUP grp consumer2 COUNT 5 STREAMS mystream ">"
        verify_stream_tracking mystream
    }

    test {XACK tracking} {
        r DEL mystream
        set ids {}
        for {set i 0} {$i < 10} {incr i} {
            lappend ids [r XADD mystream "*" f "v_$i"]
        }
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp consumer1 COUNT 10 STREAMS mystream ">"
        verify_stream_tracking mystream
        # ACK half
        for {set i 0} {$i < 5} {incr i} {
            r XACK mystream grp [lindex $ids $i]
        }
        verify_stream_tracking mystream
        # ACK rest
        for {set i 5} {$i < 10} {incr i} {
            r XACK mystream grp [lindex $ids $i]
        }
        verify_stream_tracking mystream
    }

    test {XGROUP CREATECONSUMER and DELCONSUMER tracking} {
        r DEL mystream
        r XADD mystream "*" f v
        r XGROUP CREATE mystream grp 0
        r XGROUP CREATECONSUMER mystream grp alice
        verify_stream_tracking mystream
        r XGROUP CREATECONSUMER mystream grp bob_with_longer_name
        verify_stream_tracking mystream
        r XGROUP DELCONSUMER mystream grp alice
        verify_stream_tracking mystream
        r XGROUP DELCONSUMER mystream grp bob_with_longer_name
        verify_stream_tracking mystream
    }

    test {XGROUP DELCONSUMER with pending NACKs tracking} {
        r DEL mystream
        for {set i 0} {$i < 10} {incr i} {
            r XADD mystream "*" f "v_$i"
        }
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp myconsumer COUNT 10 STREAMS mystream ">"
        verify_stream_tracking mystream
        # Delete consumer with 10 pending NACKs
        r XGROUP DELCONSUMER mystream grp myconsumer
        verify_stream_tracking mystream
    }

    test {XGROUP DESTROY with consumers and NACKs tracking} {
        r DEL mystream
        for {set i 0} {$i < 20} {incr i} {
            r XADD mystream "*" f "v_$i"
        }
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 COUNT 10 STREAMS mystream ">"
        r XREADGROUP GROUP grp c2 COUNT 10 STREAMS mystream ">"
        verify_stream_tracking mystream
        # Destroy the whole group
        r XGROUP DESTROY mystream grp
        verify_stream_tracking mystream
    }

    test {XCLAIM tracking} {
        r DEL mystream
        for {set i 0} {$i < 10} {incr i} {
            r XADD mystream "*" f "v_$i"
        }
        r XGROUP CREATE mystream grp 0
        set entries [r XREADGROUP GROUP grp consumer1 COUNT 10 STREAMS mystream ">"]
        verify_stream_tracking mystream
        # Claim all entries to a new consumer (creates consumer2)
        set ids {}
        foreach entry [lindex $entries 0 1] {
            lappend ids [lindex $entry 0]
        }
        r XCLAIM mystream grp consumer2 0 {*}$ids
        verify_stream_tracking mystream
    }

    test {XAUTOCLAIM tracking} {
        r DEL mystream
        for {set i 0} {$i < 10} {incr i} {
            r XADD mystream "*" f "v_$i"
        }
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp old_consumer COUNT 10 STREAMS mystream ">"
        verify_stream_tracking mystream
        # Auto-claim to new consumer
        after 10 ;# small delay so min-idle=0 works
        r XAUTOCLAIM mystream grp new_consumer 0 0-0 COUNT 10
        verify_stream_tracking mystream
    }

    test {XADD with MAXLEN auto-trim tracking} {
        r DEL mystream
        for {set i 0} {$i < 200} {incr i} {
            r XADD mystream MAXLEN 50 "*" f "v_$i"
        }
        verify_stream_tracking mystream
    }

    test {Full lifecycle tracking} {
        r DEL mystream
        # Add entries
        for {set i 0} {$i < 50} {incr i} {
            r XADD mystream "*" name "user_$i" score $i
        }
        verify_stream_tracking mystream

        # Create groups and consumers
        r XGROUP CREATE mystream grp1 0
        r XGROUP CREATE mystream grp2 0
        verify_stream_tracking mystream

        # Deliver to consumers
        r XREADGROUP GROUP grp1 alice COUNT 20 STREAMS mystream ">"
        r XREADGROUP GROUP grp1 bob COUNT 20 STREAMS mystream ">"
        r XREADGROUP GROUP grp2 charlie COUNT 50 STREAMS mystream ">"
        verify_stream_tracking mystream

        # ACK some
        set entries [r XRANGE mystream - + COUNT 10]
        foreach entry $entries {
            r XACK mystream grp1 [lindex $entry 0]
        }
        verify_stream_tracking mystream

        # Delete a consumer with pending
        r XGROUP DELCONSUMER mystream grp1 bob
        verify_stream_tracking mystream

        # Destroy a group
        r XGROUP DESTROY mystream grp2
        verify_stream_tracking mystream

        # Trim
        r XTRIM mystream MAXLEN 10
        verify_stream_tracking mystream

        # Delete remaining entries
        set entries [r XRANGE mystream - +]
        foreach entry $entries {
            r XDEL mystream [lindex $entry 0]
        }
        verify_stream_tracking mystream
    }
}
