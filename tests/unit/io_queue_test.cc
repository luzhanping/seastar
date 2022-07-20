/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Copyright (C) 2021 ScyllaDB
 */

#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/file.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/core/internal/io_request.hh>
#include <seastar/core/internal/io_sink.hh>
#include <seastar/util/internal/iovec_utils.hh>

using namespace seastar;

template <size_t Len>
struct fake_file {
    int data[Len] = {};

    static internal::io_request make_write_req(size_t idx, int* buf) {
        return internal::io_request::make_write(0, idx, buf, 1, false);
    }

    void execute_write_req(internal::io_request& rq, io_completion* desc) {
        data[rq.pos()] = *(reinterpret_cast<int*>(rq.address()));
        desc->complete_with(rq.size());
    }
};

struct io_queue_for_tests {
    io_group_ptr group;
    internal::io_sink sink;
    io_queue queue;
    timer<> kicker;

    io_queue_for_tests()
        : group(std::make_shared<io_group>(io_queue::config{0}))
        , sink()
        , queue(group, sink)
        , kicker([this] { kick(); })
    {
        kicker.arm_periodic(std::chrono::microseconds(500));
    }

    void kick() {
        for (auto&& fg : group->_fgs) {
            fg->replenish_capacity(std::chrono::steady_clock::now());
        }
    }
};

SEASTAR_THREAD_TEST_CASE(test_basic_flow) {
    io_queue_for_tests tio;
    fake_file<1> file;

    auto val = std::make_unique<int>(42);
    auto f = tio.queue.queue_request(default_priority_class(), internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 0), file.make_write_req(0, val.get()), nullptr, {})
    .then([&file] (size_t len) {
        BOOST_REQUIRE(file.data[0] == 42);
    });

    seastar::sleep(std::chrono::milliseconds(500)).get();
    tio.queue.poll_io_queue();
    tio.sink.drain([&file] (internal::io_request& rq, io_completion* desc) -> bool {
        file.execute_write_req(rq, desc);
        return true;
    });

    f.get();
}

SEASTAR_THREAD_TEST_CASE(test_intent_safe_ref) {
    auto get_cancelled = [] (internal::intent_reference& iref) -> bool {
        try {
            iref.retrieve();
            return false;
        } catch(seastar::cancelled_error& err) {
            return true;
        }
    };

    io_intent intent, intent_x;

    internal::intent_reference ref_orig(&intent);
    BOOST_REQUIRE(ref_orig.retrieve() == &intent);

    // Test move armed
    internal::intent_reference ref_armed(std::move(ref_orig));
    BOOST_REQUIRE(ref_orig.retrieve() == nullptr);
    BOOST_REQUIRE(ref_armed.retrieve() == &intent);

    internal::intent_reference ref_armed_2(&intent_x);
    ref_armed_2 = std::move(ref_armed);
    BOOST_REQUIRE(ref_armed.retrieve() == nullptr);
    BOOST_REQUIRE(ref_armed_2.retrieve() == &intent);

    intent.cancel();
    BOOST_REQUIRE(get_cancelled(ref_armed_2));

    // Test move cancelled
    internal::intent_reference ref_cancelled(std::move(ref_armed_2));
    BOOST_REQUIRE(ref_armed_2.retrieve() == nullptr);
    BOOST_REQUIRE(get_cancelled(ref_cancelled));

    internal::intent_reference ref_cancelled_2(&intent_x);
    ref_cancelled_2 = std::move(ref_cancelled);
    BOOST_REQUIRE(ref_cancelled.retrieve() == nullptr);
    BOOST_REQUIRE(get_cancelled(ref_cancelled_2));

    // Test move empty
    internal::intent_reference ref_empty(std::move(ref_orig));
    BOOST_REQUIRE(ref_empty.retrieve() == nullptr);

    internal::intent_reference ref_empty_2(&intent_x);
    ref_empty_2 = std::move(ref_empty);
    BOOST_REQUIRE(ref_empty_2.retrieve() == nullptr);
}

static constexpr int nr_requests = 24;

SEASTAR_THREAD_TEST_CASE(test_io_cancellation) {
    fake_file<nr_requests> file;

    io_queue_for_tests tio;
    io_priority_class pc0 = io_priority_class::register_one("a", 100);
    io_priority_class pc1 = io_priority_class::register_one("b", 100);

    size_t idx = 0;
    int val = 100;

    io_intent live, dead;

    std::vector<future<>> finished;
    std::vector<future<>> cancelled;

    auto queue_legacy_request = [&] (io_queue_for_tests& q, io_priority_class& pc) {
        auto buf = std::make_unique<int>(val);
        auto f = q.queue.queue_request(pc, internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 0), file.make_write_req(idx, buf.get()), nullptr, {})
            .then([&file, idx, val, buf = std::move(buf)] (size_t len) {
                BOOST_REQUIRE(file.data[idx] == val);
                return make_ready_future<>();
            });
        finished.push_back(std::move(f));
        idx++;
        val++;
    };

    auto queue_live_request = [&] (io_queue_for_tests& q, io_priority_class& pc) {
        auto buf = std::make_unique<int>(val);
        auto f = q.queue.queue_request(pc, internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 0), file.make_write_req(idx, buf.get()), &live, {})
            .then([&file, idx, val, buf = std::move(buf)] (size_t len) {
                BOOST_REQUIRE(file.data[idx] == val);
                return make_ready_future<>();
            });
        finished.push_back(std::move(f));
        idx++;
        val++;
    };

    auto queue_dead_request = [&] (io_queue_for_tests& q, io_priority_class& pc) {
        auto buf = std::make_unique<int>(val);
        auto f = q.queue.queue_request(pc, internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 0), file.make_write_req(idx, buf.get()), &dead, {})
            .then_wrapped([buf = std::move(buf)] (auto&& f) {
                try {
                    f.get();
                    BOOST_REQUIRE(false);
                } catch(...) {}
                return make_ready_future<>();
            })
            .then([&file, idx] () {
                BOOST_REQUIRE(file.data[idx] == 0);
            });
        cancelled.push_back(std::move(f));
        idx++;
        val++;
    };

    auto seed = std::random_device{}();
    std::default_random_engine reng(seed);
    std::uniform_int_distribution<> dice(0, 5);

    for (int i = 0; i < nr_requests; i++) {
        int pc = dice(reng) % 2;
        if (dice(reng) < 3) {
            fmt::print("queue live req to pc {}\n", pc);
            queue_live_request(tio, pc == 0 ? pc0 : pc1);
        } else if (dice(reng) < 5) {
            fmt::print("queue dead req to pc {}\n", pc);
            queue_dead_request(tio, pc == 0 ? pc0 : pc1);
        } else {
            fmt::print("queue legacy req to pc {}\n", pc);
            queue_legacy_request(tio, pc == 0 ? pc0 : pc1);
        }
    }

    dead.cancel();

    // cancelled requests must resolve right at once

    when_all_succeed(cancelled.begin(), cancelled.end()).get();

    seastar::sleep(std::chrono::milliseconds(500)).get();
    tio.queue.poll_io_queue();
    tio.sink.drain([&file] (internal::io_request& rq, io_completion* desc) -> bool {
        file.execute_write_req(rq, desc);
        return true;
    });

    when_all_succeed(finished.begin(), finished.end()).get();
}

SEASTAR_TEST_CASE(test_request_buffer_split) {
    auto ensure = [] (const std::vector<internal::io_request::part>& parts, const internal::io_request& req, int idx, uint64_t pos, size_t size, uintptr_t mem) {
        BOOST_REQUIRE(parts[idx].req.opcode() == req.opcode());
        BOOST_REQUIRE_EQUAL(parts[idx].req.fd(), req.fd());
        BOOST_REQUIRE_EQUAL(parts[idx].req.pos(), pos);
        BOOST_REQUIRE_EQUAL(parts[idx].req.size(), size);
        BOOST_REQUIRE_EQUAL(parts[idx].req.address(), reinterpret_cast<void*>(mem));
        BOOST_REQUIRE_EQUAL(parts[idx].req.nowait_works(), req.nowait_works());
        BOOST_REQUIRE_EQUAL(parts[idx].iovecs.size(), 0);
        BOOST_REQUIRE_EQUAL(parts[idx].size, parts[idx].req.size());
    };

    // No split
    {
        internal::io_request req = internal::io_request::make_read(5, 13, reinterpret_cast<void*>(0x420), 17, true);
        auto parts = req.split(21);
        BOOST_REQUIRE_EQUAL(parts.size(), 1);
        ensure(parts, req, 0, 13, 17, 0x420);
    }

    // Without tail
    {
        internal::io_request req = internal::io_request::make_read(7, 24, reinterpret_cast<void*>(0x4321), 24, true);
        auto parts = req.split(12);
        BOOST_REQUIRE_EQUAL(parts.size(), 2);
        ensure(parts, req, 0, 24,      12, 0x4321);
        ensure(parts, req, 1, 24 + 12, 12, 0x4321 + 12);
    }

    // With tail
    {
        internal::io_request req = internal::io_request::make_read(9, 42, reinterpret_cast<void*>(0x1234), 33, true);
        auto parts = req.split(13);
        BOOST_REQUIRE_EQUAL(parts.size(), 3);
        ensure(parts, req, 0, 42,      13, 0x1234);
        ensure(parts, req, 1, 42 + 13, 13, 0x1234 + 13);
        ensure(parts, req, 2, 42 + 26,  7, 0x1234 + 26);
    }

    return make_ready_future<>();
}

static void show_request(const internal::io_request& req, void* buf_off, std::string pfx = "") {
    if (!seastar_logger.is_enabled(log_level::trace)) {
        return;
    }

    seastar_logger.trace("{}{} iovecs on req:", pfx, req.iov_len());
    for (unsigned i = 0; i < req.iov_len(); i++) {
        seastar_logger.trace("{}  base={} len={}", pfx, reinterpret_cast<uintptr_t>(req.iov()[i].iov_base) - reinterpret_cast<uintptr_t>(buf_off), req.iov()[i].iov_len);
    }
}

static void show_request_parts(const std::vector<internal::io_request::part>& parts, void* buf_off) {
    if (!seastar_logger.is_enabled(log_level::trace)) {
        return;
    }

    seastar_logger.trace("{} parts", parts.size());
    for (const auto& p : parts) {
        seastar_logger.trace("  size={} iovecs={}", p.size, p.iovecs.size());
        seastar_logger.trace("  {} iovecs on part:", p.iovecs.size());
        for (const auto& iov : p.iovecs) {
            seastar_logger.trace("    base={} len={}", reinterpret_cast<uintptr_t>(iov.iov_base) - reinterpret_cast<uintptr_t>(buf_off), iov.iov_len);
        }
        show_request(p.req, buf_off, "  ");
    }
}

SEASTAR_TEST_CASE(test_request_iovec_split) {
    char large_buffer[1025];

    auto clear_buffer = [&large_buffer] {
        memset(large_buffer, 0, sizeof(large_buffer));
    };

    auto bump_buffer = [] (const std::vector<::iovec>& vecs) {
        for (auto&& v : vecs) {
            for (unsigned i = 0; i < v.iov_len; i++) {
                (reinterpret_cast<char*>(v.iov_base))[i]++;
            }
        }
    };

    auto check_buffer = [&large_buffer] (size_t len, char value) {
        assert(len < sizeof(large_buffer));
        bool fill_match = true;
        bool train_match = true;
        for (unsigned i = 0; i < sizeof(large_buffer); i++) {
            if (i < len) {
                if (large_buffer[i] != value) {
                    fill_match = false;
                }
            } else {
                if (large_buffer[i] != '\0') {
                    train_match = false;
                }
            }
        }
        BOOST_REQUIRE_EQUAL(fill_match, true);
        BOOST_REQUIRE_EQUAL(train_match, true);
    };

    auto ensure = [] (const std::vector<internal::io_request::part>& parts, const internal::io_request& req, int idx, uint64_t pos) {
        BOOST_REQUIRE(parts[idx].req.opcode() == req.opcode());
        BOOST_REQUIRE_EQUAL(parts[idx].req.fd(), req.fd());
        BOOST_REQUIRE_EQUAL(parts[idx].req.pos(), pos);
        BOOST_REQUIRE_EQUAL(parts[idx].req.iov_len(), parts[idx].iovecs.size());
        BOOST_REQUIRE_EQUAL(parts[idx].req.nowait_works(), req.nowait_works());
        BOOST_REQUIRE_EQUAL(parts[idx].size, internal::iovec_len(parts[idx].iovecs));

        for (unsigned iov = 0; iov < parts[idx].iovecs.size(); iov++) {
            BOOST_REQUIRE_EQUAL(parts[idx].req.iov()[iov].iov_base, parts[idx].iovecs[iov].iov_base);
            BOOST_REQUIRE_EQUAL(parts[idx].req.iov()[iov].iov_len, parts[idx].iovecs[iov].iov_len);
        }
    };

    std::default_random_engine& reng = testing::local_random_engine;
    auto dice = std::uniform_int_distribution<uint16_t>(1, 31);
    auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    uint64_t iter = 0;
    unsigned no_splits = 0;
    unsigned no_tails = 0;

    do {
        seastar_logger.debug("===== iter {} =====", iter++);
        std::vector<::iovec> vecs;
        unsigned nr_vecs = dice(reng) % 13 + 1;
        seastar_logger.debug("Generate {} iovecs", nr_vecs);
        size_t total = 0;
        for (unsigned i = 0; i < nr_vecs; i++) {
            ::iovec iov;
            iov.iov_base = reinterpret_cast<void*>(large_buffer + total);
            iov.iov_len = dice(reng);
            assert(iov.iov_len != 0);
            total += iov.iov_len;
            vecs.push_back(std::move(iov));
        }

        assert(total > 0);
        clear_buffer();
        bump_buffer(vecs);
        check_buffer(total, 1);

        size_t file_off = dice(reng);
        internal::io_request req = internal::io_request::make_readv(5, file_off, vecs, true);

        show_request(req, large_buffer);

        size_t max_len = dice(reng) * 3;
        unsigned nr_parts = (total + max_len - 1) / max_len;
        seastar_logger.debug("Split {} into {}-bytes ({} parts)", total, max_len, nr_parts);
        auto parts = req.split(max_len);
        show_request_parts(parts, large_buffer);
        BOOST_REQUIRE_EQUAL(parts.size(), nr_parts);

        size_t parts_total = 0;
        for (unsigned p = 0; p < nr_parts; p++) {
            ensure(parts, req, p, file_off + parts_total);
            if (p < nr_parts - 1) {
                BOOST_REQUIRE_EQUAL(parts[p].size, max_len);
            }
            parts_total += parts[p].size;
            bump_buffer(parts[p].iovecs);
        }
        BOOST_REQUIRE_EQUAL(parts_total, total);
        check_buffer(total, 2);

        if (parts.size() == 1) {
            no_splits++;
        }
        if (parts.back().size == max_len) {
            no_tails++;
        }
    } while (std::chrono::steady_clock::now() < stop || iter < 32 || no_splits < 16 || no_tails < 16);

    seastar_logger.info("{} iters ({} no-splits, {} no-tails)", iter, no_splits, no_tails);

    return make_ready_future<>();
}
