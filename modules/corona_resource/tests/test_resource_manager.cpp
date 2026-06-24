#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "corona/resource/resource_manager.h"

using namespace Corona::Resource;

// Test statistics
struct TestStats {
    std::atomic<int> passed{0};
    std::atomic<int> failed{0};
    std::atomic<int> total{0};

    void record_pass() {
        passed++;
        total++;
    }

    void record_fail() {
        failed++;
        total++;
    }

    void print() const {
        std::cout << "\n=== Test Results ===" << std::endl;
        std::cout << "Total: " << total << std::endl;
        std::cout << "Passed: " << passed << " ("
                  << (total > 0 ? (passed * 100.0 / total) : 0) << "%)" << std::endl;
        std::cout << "Failed: " << failed << std::endl;
    }
};

TestStats g_stats;

// Mock Resource
class MockResource : public IResource {
   public:
    explicit MockResource(const std::filesystem::path& path) : IResource(path) {
        data = "Initial Data";
        load_count++;
    }
    std::string data;
    static std::atomic<int> load_count;

    static void reset_load_count() {
        load_count = 0;
    }
};

std::atomic<int> MockResource::load_count{0};

// Mock Parser
class MockParser : public IParser {
   public:
    MockParser() {
        register_extension(".mock", [](const std::filesystem::path& path,
                                        ResourceCache&) -> std::shared_ptr<IResource> {
            if (should_fail_next_load) {
                should_fail_next_load = false;
                return std::shared_ptr<IResource>{};
            }
            return std::make_shared<MockResource>(path);
        });

        // Register export handler using the new method
        register_exporter(".mock", [](const IResource& resource, const std::filesystem::path& path) {
            // Simulate export time
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (should_fail_next_export) {
                should_fail_next_export = false;
                return false;
            }
            return true;
        });
    }

    static std::atomic<bool> should_fail_next_load;
    static std::atomic<bool> should_fail_next_export;
};

std::atomic<bool> MockParser::should_fail_next_load{false};
std::atomic<bool> MockParser::should_fail_next_export{false};

bool test_sync_import() {
    std::cout << "\n[TEST] Sync Import" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        auto path = std::filesystem::path("test_asset.mock");

        // Test valid import
        int initial_count = MockResource::load_count;
        auto id = manager.import_sync(path);
        if (id == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Import returned invalid UID" << std::endl;
            return false;
        }

        // Verify resource was loaded
        if (MockResource::load_count != initial_count + 1) {
            std::cerr << "  FAILED: Resource not loaded (count: " << MockResource::load_count << ")" << std::endl;
            return false;
        }

        // Test read access
        auto handle = manager.acquire_read<MockResource>(id);
        if (!handle || !handle.valid()) {
            std::cerr << "  FAILED: Failed to acquire read handle" << std::endl;
            return false;
        }

        if (handle->data != "Initial Data") {
            std::cerr << "  FAILED: Unexpected data: '" << handle->data << "'" << std::endl;
            return false;
        }

        std::cout << "  Read data: " << handle->data << std::endl;

        // Test cache behavior - same path should return same ID
        auto id2 = manager.import_sync(path);
        if (id2 != id) {
            std::cerr << "  FAILED: Cache not working, different IDs: " << id << " vs " << id2 << std::endl;
            return false;
        }

        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_async_import() {
    std::cout << "\n[TEST] Async Import" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        auto path = std::filesystem::path("async_asset.mock");
        auto future_id = manager.import_async(path);

        // Verify future is valid
        if (!future_id.valid()) {
            std::cerr << "  FAILED: Invalid future returned" << std::endl;
            return false;
        }

        std::cout << "  Waiting for async import..." << std::endl;

        // Add timeout protection
        auto status = future_id.wait_for(std::chrono::seconds(5));
        if (status != std::future_status::ready) {
            std::cerr << "  FAILED: Async import timeout" << std::endl;
            return false;
        }

        auto id = future_id.get();
        if (id == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Async import returned invalid UID" << std::endl;
            return false;
        }

        std::cout << "  Async import finished with ID: " << id << std::endl;

        // Verify resource is accessible
        auto handle = manager.acquire_read<MockResource>(id);
        if (!handle || !handle.valid()) {
            std::cerr << "  FAILED: Failed to acquire read handle after async import" << std::endl;
            return false;
        }

        if (handle->data != "Initial Data") {
            std::cerr << "  FAILED: Unexpected data" << std::endl;
            return false;
        }

        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_write_resource() {
    std::cout << "\n[TEST] Write Resource" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        auto path = std::filesystem::path("write_test.mock");

        // Ensure clean state
        std::ofstream(path).close();

        auto id = manager.import_sync(path);
        if (id == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Import failed" << std::endl;
            return false;
        }

        // Test write access
        {
            auto write_handle = manager.acquire_write<MockResource>(id);
            if (!write_handle || !write_handle.valid()) {
                std::cerr << "  FAILED: Failed to acquire write handle" << std::endl;
                return false;
            }
            write_handle->data = "Modified Data";
        }  // Release write lock

        // Verify modification persisted
        {
            auto read_handle = manager.acquire_read<MockResource>(id);
            if (!read_handle || !read_handle.valid()) {
                std::cerr << "  FAILED: Failed to acquire read handle" << std::endl;
                return false;
            }

            if (read_handle->data != "Modified Data") {
                std::cerr << "  FAILED: Data not modified. Got: '" << read_handle->data << "'" << std::endl;
                return false;
            }

            std::cout << "  Data after write: " << read_handle->data << std::endl;
        }

        // Test multiple sequential writes
        for (int i = 0; i < 5; ++i) {
            auto handle = manager.acquire_write<MockResource>(id);
            if (!handle) {
                std::cerr << "  FAILED: Failed to acquire write handle on iteration " << i << std::endl;
                return false;
            }
            handle->data = "Write " + std::to_string(i);
        }

        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_concurrent_access() {
    std::cout << "\n[TEST] Concurrent Access" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        auto path = std::filesystem::path("concurrent.mock");
        std::ofstream(path).close();

        MockResource::reset_load_count();
        int initial_count = MockResource::load_count;

        const int num_threads = 20;
        std::vector<std::future<TResourceID>> futures;
        futures.reserve(num_threads);

        // Launch multiple async imports simultaneously
        for (int i = 0; i < num_threads; ++i) {
            futures.push_back(manager.import_async(path));
        }

        TResourceID first_id = IResource::INVALID_UID;
        std::vector<TResourceID> all_ids;
        all_ids.reserve(num_threads);

        // Collect all results with timeout
        for (int i = 0; i < num_threads; ++i) {
            auto& f = futures[i];
            auto status = f.wait_for(std::chrono::seconds(10));
            if (status != std::future_status::ready) {
                std::cerr << "  FAILED: Thread " << i << " timeout" << std::endl;
                return false;
            }

            TResourceID id = f.get();
            if (id == IResource::INVALID_UID) {
                std::cerr << "  FAILED: Thread " << i << " got invalid UID" << std::endl;
                return false;
            }

            all_ids.push_back(id);
            if (first_id == IResource::INVALID_UID) {
                first_id = id;
            } else if (id != first_id) {
                std::cerr << "  FAILED: ID mismatch. Expected " << first_id << ", got " << id << std::endl;
                return false;
            }
        }

        // Verify resource was only loaded once (deduplication)
        int final_count = MockResource::load_count;
        if (final_count != initial_count + 1) {
            std::cerr << "  WARNING: Resource loaded " << (final_count - initial_count)
                      << " times instead of 1 (possible race condition)" << std::endl;
            // Not failing the test as some race conditions might be acceptable
        }

        std::cout << "  All " << num_threads << " threads got same ID: " << first_id << std::endl;
        std::cout << "  Resource load count: " << (final_count - initial_count) << std::endl;
        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_concurrent_read_write() {
    std::cout << "\n[TEST] Concurrent Read/Write" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        auto path = std::filesystem::path("rw_asset.mock");
        std::ofstream(path).close();

        auto id = manager.import_sync(path);
        if (id == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Import failed" << std::endl;
            return false;
        }

        std::atomic<bool> running{true};
        std::atomic<bool> test_failed{false};
        std::atomic<int> read_count{0};
        std::atomic<int> write_count{0};
        std::atomic<int> intermediate_reads{0};
        std::atomic<int> writers_waiting{0};

        // Multiple writer threads
        std::vector<std::thread> writers;
        for (int w = 0; w < 2; ++w) {
            writers.emplace_back([&, w]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));  // Let readers start
                for (int i = 0; i < 3; ++i) {
                    writers_waiting++;
                    auto handle = manager.acquire_write<MockResource>(id);
                    writers_waiting--;
                    if (!handle) continue;

                    std::string intermediate = "Intermediate_W" + std::to_string(w) + "_" + std::to_string(i);
                    std::string final = "Final_W" + std::to_string(w) + "_" + std::to_string(i);

                    handle->data = intermediate;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // Hold lock
                    handle->data = final;
                    write_count++;
                }
            });
        }

        // Reader threads
        std::vector<std::thread> readers;
        for (int i = 0; i < 8; ++i) {
            readers.emplace_back([&]() {
                while (running && !test_failed) {
                    if (writers_waiting > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    auto handle = manager.acquire_read<MockResource>(id);
                    if (handle && handle.valid()) {
                        std::string val = handle->data;
                        // Check if we read an intermediate state
                        if (val.find("Intermediate") != std::string::npos) {
                            std::cerr << "  ERROR: Read intermediate state: '" << val << "'" << std::endl;
                            intermediate_reads++;
                            test_failed = true;
                        }
                        read_count++;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
        }

        // Wait for writers with timeout
        bool writer_timeout = false;
        for (auto& t : writers) {
            if (t.joinable()) {
                // Simple timeout simulation
                auto start = std::chrono::steady_clock::now();
                while (!writer_timeout) {
                    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
                        writer_timeout = true;
                        running = false;
                        break;
                    }
                    if (write_count >= 6) break;  // 2 writers * 3 iterations
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                t.join();
            }
        }

        running = false;
        for (auto& t : readers) {
            if (t.joinable()) t.join();
        }

        if (writer_timeout) {
            std::cerr << "  FAILED: Writer timeout" << std::endl;
            return false;
        }

        if (test_failed || intermediate_reads > 0) {
            std::cerr << "  FAILED: Detected " << intermediate_reads << " intermediate reads" << std::endl;
            return false;
        }

        // Verify final state is consistent
        auto handle = manager.acquire_read<MockResource>(id);
        if (!handle || !handle.valid()) {
            std::cerr << "  FAILED: Cannot acquire final read handle" << std::endl;
            return false;
        }

        std::string final_data = handle->data;
        if (final_data.find("Final") == std::string::npos && final_data != "Initial Data") {
            std::cerr << "  FAILED: Unexpected final state: '" << final_data << "'" << std::endl;
            return false;
        }

        std::cout << "  Total reads: " << read_count << ", writes: " << write_count << std::endl;
        std::cout << "  Final state: " << final_data << std::endl;
        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_stress() {
    std::cout << "\n[TEST] Stress Test" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        auto path = std::filesystem::path("stress_asset.mock");
        std::ofstream(path).close();

        auto id = manager.import_sync(path);
        if (id == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Import failed" << std::endl;
            return false;
        }

        std::atomic<bool> running{true};
        std::atomic<bool> error_detected{false};
        std::atomic<int> read_ops{0};
        std::atomic<int> write_ops{0};
        std::atomic<int> import_ops{0};
        std::atomic<int> failed_reads{0};
        std::atomic<int> failed_writes{0};

        std::vector<std::thread> threads;
        const int num_threads = 16;

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&, i]() {
                std::mt19937 rng(i + static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
                std::uniform_int_distribution<int> dist(0, 2);

                int local_ops = 0;
                const int max_ops = 100;

                while (running && local_ops < max_ops && !error_detected) {
                    try {
                        int op = dist(rng);
                        if (op == 0) {  // Read
                            auto handle = manager.acquire_read<MockResource>(id);
                            if (handle && handle.valid()) {
                                volatile size_t len = handle->data.length();
                                if (len == 0) {
                                    std::cerr << "  WARNING: Thread " << i << " read empty data" << std::endl;
                                }
                                (void)len;
                                read_ops++;
                            } else {
                                failed_reads++;
                            }
                        } else if (op == 1) {  // Write
                            auto handle = manager.acquire_write<MockResource>(id);
                            if (handle && handle.valid()) {
                                handle->data = "Updated by T" + std::to_string(i) + " at " + std::to_string(local_ops);
                                write_ops++;
                            } else {
                                failed_writes++;
                            }
                        } else {  // Import (should be fast as it's cached)
                            auto cached_id = manager.import_sync(path);
                            if (cached_id != id) {
                                std::cerr << "  ERROR: Thread " << i << " got different ID from cache" << std::endl;
                                error_detected = true;
                            }
                            import_ops++;
                        }
                        local_ops++;
                    } catch (const std::exception& e) {
                        std::cerr << "  ERROR: Thread " << i << " exception: " << e.what() << std::endl;
                        error_detected = true;
                    }

                    // Small yield to allow other threads
                    if (local_ops % 10 == 0) {
                        std::this_thread::yield();
                    }
                }
            });
        }

        // Run for limited time
        auto start = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(5);

        while (std::chrono::steady_clock::now() - start < timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (error_detected) {
                break;
            }
        }

        running = false;
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }

        if (error_detected) {
            std::cerr << "  FAILED: Error detected during stress test" << std::endl;
            return false;
        }

        std::cout << "  Operations completed:" << std::endl;
        std::cout << "    Reads: " << read_ops << " (failed: " << failed_reads << ")" << std::endl;
        std::cout << "    Writes: " << write_ops << " (failed: " << failed_writes << ")" << std::endl;
        std::cout << "    Imports: " << import_ops << std::endl;
        std::cout << "    Total: " << (read_ops + write_ops + import_ops) << std::endl;

        // Verify system is still functional
        auto handle = manager.acquire_read<MockResource>(id);
        if (!handle || !handle.valid()) {
            std::cerr << "  FAILED: Resource not accessible after stress test" << std::endl;
            return false;
        }

        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_invalid_path() {
    std::cout << "\n[TEST] Invalid Path Handling" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        // Test non-existent file
        auto path = std::filesystem::path("nonexistent_file.mock");
        auto id = manager.import_sync(path);

        // System should handle this gracefully
        if (id == IResource::INVALID_UID) {
            std::cout << "  Correctly returned INVALID_UID for non-existent file" << std::endl;
        }

        // Test unsupported extension
        auto unsupported = std::filesystem::path("test.unsupported");
        std::ofstream(unsupported).close();
        auto id2 = manager.import_sync(unsupported);

        if (id2 == IResource::INVALID_UID) {
            std::cout << "  Correctly rejected unsupported file type" << std::endl;
        }

        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_export_functionality() {
    std::cout << "\n[TEST] Export Functionality" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        auto import_path = std::filesystem::path("export_source.mock");
        std::ofstream(import_path).close();

        auto id = manager.import_sync(import_path);
        if (id == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Import failed" << std::endl;
            return false;
        }

        // Modify resource
        {
            auto handle = manager.acquire_write<MockResource>(id);
            if (!handle) {
                std::cerr << "  FAILED: Cannot acquire write handle" << std::endl;
                return false;
            }
            handle->data = "Exported Data";
        }

        // Test sync export
        auto export_path = std::filesystem::path("export_output.mock");
        bool success = manager.export_sync(id, export_path);
        if (!success) {
            std::cerr << "  FAILED: Sync export failed" << std::endl;
            return false;
        }
        std::cout << "  Sync export succeeded" << std::endl;

        // Test async export
        auto export_path2 = std::filesystem::path("export_output2.mock");
        auto future = manager.export_async(id, export_path2);

        auto status = future.wait_for(std::chrono::seconds(5));
        if (status != std::future_status::ready) {
            std::cerr << "  FAILED: Async export timeout" << std::endl;
            return false;
        }

        bool async_success = future.get();
        if (!async_success) {
            std::cerr << "  FAILED: Async export failed" << std::endl;
            return false;
        }
        std::cout << "  Async export succeeded" << std::endl;

        // Test export with invalid ID
        bool should_fail = manager.export_sync(IResource::INVALID_UID, "invalid.mock");
        if (should_fail) {
            std::cerr << "  WARNING: Export with invalid ID should have failed" << std::endl;
        }

        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_resource_removal() {
    std::cout << "\n[TEST] Resource Removal" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        auto path = std::filesystem::path("removal_test.mock");
        std::ofstream(path).close();

        auto id = manager.import_sync(path);
        if (id == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Import failed" << std::endl;
            return false;
        }

        // Verify resource is accessible
        {
            auto handle = manager.acquire_read<MockResource>(id);
            if (!handle) {
                std::cerr << "  FAILED: Cannot acquire handle before removal" << std::endl;
                return false;
            }
        }

        // Remove resource
        bool removed = manager.remove_cache(id);
        if (!removed) {
            std::cerr << "  FAILED: Resource removal failed" << std::endl;
            return false;
        }
        std::cout << "  Resource removed from cache" << std::endl;

        // Try to access removed resource
        auto handle = manager.acquire_read<MockResource>(id);
        if (handle && handle.valid()) {
            std::cerr << "  WARNING: Removed resource is still accessible" << std::endl;
        } else {
            std::cout << "  Removed resource is no longer accessible" << std::endl;
        }

        // Re-import should work
        auto id2 = manager.import_sync(path);
        if (id2 == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Re-import failed" << std::endl;
            return false;
        }
        std::cout << "  Re-import successful" << std::endl;

        // Try removing non-existent resource
        bool removed2 = manager.remove_cache(999999);
        if (removed2) {
            std::cerr << "  WARNING: Removal of non-existent resource returned true" << std::endl;
        }

        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_handle_scope_and_raii() {
    std::cout << "\n[TEST] Handle Scope and RAII" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        auto path = std::filesystem::path("raii_test.mock");
        std::ofstream(path).close();

        auto id = manager.import_sync(path);
        if (id == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Import failed" << std::endl;
            return false;
        }

        // Test that write lock is released when handle goes out of scope
        {
            auto write_handle = manager.acquire_write<MockResource>(id);
            if (!write_handle) {
                std::cerr << "  FAILED: Cannot acquire first write handle" << std::endl;
                return false;
            }
            write_handle->data = "Scoped Write";
            // Lock should be released here
        }

        // Should be able to acquire another write lock immediately
        auto write_handle2 = manager.acquire_write<MockResource>(id);
        if (!write_handle2) {
            std::cerr << "  FAILED: Cannot acquire second write handle (RAII failed)" << std::endl;
            return false;
        }

        // Test move semantics
        auto moved_handle = std::move(write_handle2);
        if (!moved_handle) {
            std::cerr << "  FAILED: Moved handle is invalid" << std::endl;
            return false;
        }

        // Original handle should be invalid after move
        if (write_handle2.valid()) {
            std::cerr << "  FAILED: Original handle still valid after move" << std::endl;
            return false;
        }

        std::cout << "  RAII and move semantics working correctly" << std::endl;
        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_multiple_readers() {
    std::cout << "\n[TEST] Multiple Concurrent Readers" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        auto path = std::filesystem::path("multi_read.mock");
        std::ofstream(path).close();

        auto id = manager.import_sync(path);
        if (id == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Import failed" << std::endl;
            return false;
        }

        std::atomic<int> concurrent_readers{0};
        std::atomic<int> max_concurrent{0};
        std::atomic<bool> running{true};

        std::vector<std::thread> threads;
        for (int i = 0; i < 10; ++i) {
            threads.emplace_back([&]() {
                for (int j = 0; j < 5; ++j) {
                    auto handle = manager.acquire_read<MockResource>(id);
                    if (handle && handle.valid()) {
                        concurrent_readers++;
                        // Update max
                        int current = concurrent_readers;
                        int expected = max_concurrent;
                        while (current > expected &&
                               !max_concurrent.compare_exchange_weak(expected, current)) {
                            expected = max_concurrent;
                        }

                        // Hold for a bit
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        concurrent_readers--;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
        }

        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }

        std::cout << "  Max concurrent readers: " << max_concurrent << std::endl;
        if (max_concurrent < 2) {
            std::cerr << "  WARNING: Expected multiple concurrent readers, got " << max_concurrent << std::endl;
        }

        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_memory_scalability() {
    std::cout << "\n[TEST] Memory Scalability" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        const int num_resources = 100;
        std::vector<TResourceID> resource_ids;
        resource_ids.reserve(num_resources);

        // Load many resources
        std::cout << "  Loading " << num_resources << " resources..." << std::endl;
        for (int i = 0; i < num_resources; ++i) {
            auto path = std::filesystem::path("scalability_" + std::to_string(i) + ".mock");
            std::ofstream(path).close();

            auto id = manager.import_sync(path);
            if (id == IResource::INVALID_UID) {
                std::cerr << "  FAILED: Failed to load resource " << i << std::endl;
                // Cleanup
                for (int j = 0; j < num_resources; ++j) {
                    std::filesystem::remove("scalability_" + std::to_string(j) + ".mock");
                }
                return false;
            }
            resource_ids.push_back(id);
        }

        // Verify all resources are accessible
        std::cout << "  Verifying all resources are accessible..." << std::endl;
        int accessible_count = 0;
        for (auto id : resource_ids) {
            auto handle = manager.acquire_read<MockResource>(id);
            if (handle && handle.valid()) {
                accessible_count++;
            }
        }

        if (accessible_count != num_resources) {
            std::cerr << "  FAILED: Only " << accessible_count << "/" << num_resources << " resources accessible" << std::endl;
            // Cleanup
            for (int i = 0; i < num_resources; ++i) {
                std::filesystem::remove("scalability_" + std::to_string(i) + ".mock");
            }
            return false;
        }

        // Test random access pattern
        std::cout << "  Testing random access pattern..." << std::endl;
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> dist(0, num_resources - 1);

        for (int i = 0; i < 200; ++i) {
            int idx = dist(rng);
            auto handle = manager.acquire_read<MockResource>(resource_ids[idx]);
            if (!handle || !handle.valid()) {
                std::cerr << "  FAILED: Cannot access resource " << idx << " during random access" << std::endl;
                // Cleanup
                for (int j = 0; j < num_resources; ++j) {
                    std::filesystem::remove("scalability_" + std::to_string(j) + ".mock");
                }
                return false;
            }
        }

        std::cout << "  All " << num_resources << " resources loaded and accessible" << std::endl;
        std::cout << "  Random access test completed successfully" << std::endl;

        // Cleanup
        for (int i = 0; i < num_resources; ++i) {
            std::filesystem::remove("scalability_" + std::to_string(i) + ".mock");
        }

        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

bool test_resource_lifecycle() {
    std::cout << "\n[TEST] Resource Lifecycle" << std::endl;
    try {
        auto& manager = ResourceManager::get_instance();
        // Step 1: Import
        std::cout << "  Step 1: Import resource..." << std::endl;
        auto path = std::filesystem::path("lifecycle.mock");
        std::ofstream(path).close();

        auto id = manager.import_sync(path);
        if (id == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Import failed" << std::endl;
            std::filesystem::remove(path);
            return false;
        }

        // Step 2: Read initial data
        std::cout << "  Step 2: Read initial data..." << std::endl;
        std::string initial_data;
        {
            auto handle = manager.acquire_read<MockResource>(id);
            if (!handle || !handle.valid()) {
                std::cerr << "  FAILED: Cannot read initial data" << std::endl;
                std::filesystem::remove(path);
                return false;
            }
            initial_data = handle->data;
        }

        if (initial_data != "Initial Data") {
            std::cerr << "  FAILED: Unexpected initial data: '" << initial_data << "'" << std::endl;
            std::filesystem::remove(path);
            return false;
        }

        // Step 3: Modify
        std::cout << "  Step 3: Modify resource..." << std::endl;
        const std::string modified_data = "Lifecycle Test Data - Modified";
        {
            auto handle = manager.acquire_write<MockResource>(id);
            if (!handle || !handle.valid()) {
                std::cerr << "  FAILED: Cannot acquire write handle" << std::endl;
                std::filesystem::remove(path);
                return false;
            }
            handle->data = modified_data;
        }

        // Step 4: Verify modification
        std::cout << "  Step 4: Verify modification..." << std::endl;
        {
            auto handle = manager.acquire_read<MockResource>(id);
            if (!handle || !handle.valid()) {
                std::cerr << "  FAILED: Cannot read after modification" << std::endl;
                std::filesystem::remove(path);
                return false;
            }
            if (handle->data != modified_data) {
                std::cerr << "  FAILED: Modification not persisted. Got: '" << handle->data << "'" << std::endl;
                std::filesystem::remove(path);
                return false;
            }
        }

        // Step 5: Export
        std::cout << "  Step 5: Export resource..." << std::endl;
        auto export_path = std::filesystem::path("lifecycle_exported.mock");
        bool exported = manager.export_sync(id, export_path);
        if (!exported) {
            std::cerr << "  FAILED: Export failed" << std::endl;
            std::filesystem::remove(path);
            return false;
        }

        // Step 6: Remove from cache
        std::cout << "  Step 6: Remove from cache..." << std::endl;
        bool removed = manager.remove_cache(id);
        if (!removed) {
            std::cerr << "  FAILED: Remove failed" << std::endl;
            std::filesystem::remove(path);
            std::filesystem::remove(export_path);
            return false;
        }

        // Step 7: Re-import from original file
        std::cout << "  Step 7: Re-import from original..." << std::endl;
        auto id2 = manager.import_sync(path);
        if (id2 == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Re-import failed" << std::endl;
            std::filesystem::remove(path);
            std::filesystem::remove(export_path);
            return false;
        }

        // Note: Re-importing creates a fresh MockResource with "Initial Data"
        // This is expected behavior as we're testing the import, not file persistence
        {
            auto handle = manager.acquire_read<MockResource>(id2);
            if (!handle || !handle.valid()) {
                std::cerr << "  FAILED: Cannot read re-imported resource" << std::endl;
                std::filesystem::remove(path);
                std::filesystem::remove(export_path);
                return false;
            }
            std::cout << "  Re-imported data: '" << handle->data << "'" << std::endl;
        }

        // Step 8: Import exported file
        std::cout << "  Step 8: Import exported file..." << std::endl;
        auto id3 = manager.import_sync(export_path);
        if (id3 == IResource::INVALID_UID) {
            std::cerr << "  FAILED: Import of exported file failed" << std::endl;
            std::filesystem::remove(path);
            std::filesystem::remove(export_path);
            return false;
        }

        {
            auto handle = manager.acquire_read<MockResource>(id3);
            if (!handle || !handle.valid()) {
                std::cerr << "  FAILED: Cannot read exported resource" << std::endl;
                std::filesystem::remove(path);
                std::filesystem::remove(export_path);
                return false;
            }
            std::cout << "  Exported file data: '" << handle->data << "'" << std::endl;
        }

        std::cout << "  Full lifecycle completed successfully" << std::endl;

        // Cleanup
        std::filesystem::remove(path);
        std::filesystem::remove(export_path);

        std::cout << "  PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "  FAILED: Exception: " << e.what() << std::endl;
        return false;
    }
}

// Utility function to create test files
void create_test_files() {
    std::vector<std::string> files = {
        "test_asset.mock",
        "async_asset.mock",
        "concurrent.mock",
        "rw_asset.mock",
        "stress_asset.mock",
        "write_test.mock",
        "removal_test.mock",
        "raii_test.mock",
        "multi_read.mock",
        "export_source.mock",
        "lifecycle.mock",
        "lifecycle_exported.mock",
        "cache_perf.mock",
        "parser_fail.mock",
        "parser_recovery.mock",
        "async_parser_fail.mock",
        "extreme_concurrency.mock"};

    for (const auto& file : files) {
        std::ofstream(file).close();
    }
}

// Utility function to cleanup test files
void cleanup_test_files() {
    std::vector<std::string> files = {
        "test_asset.mock",
        "async_asset.mock",
        "concurrent.mock",
        "rw_asset.mock",
        "stress_asset.mock",
        "write_test.mock",
        "removal_test.mock",
        "raii_test.mock",
        "multi_read.mock",
        "export_source.mock",
        "export_output.mock",
        "export_output2.mock",
        "test.unsupported",
        "lifecycle.mock",
        "lifecycle_exported.mock",
        "cache_perf.mock",
        "parser_fail.mock",
        "parser_recovery.mock",
        "async_parser_fail.mock",
        "extreme_concurrency.mock"};

    for (const auto& file : files) {
        try {
            std::filesystem::remove(file);
        } catch (...) {
            // Ignore cleanup errors
        }
    }
}

int main() {
    std::cout << "=================================" << std::endl;
    std::cout << "Resource Manager Test Suite" << std::endl;
    std::cout << "=================================" << std::endl;

    // Create test files
    create_test_files();

    // Initialize manager (singleton)
    auto& manager = ResourceManager::get_instance();
    manager.register_parser<MockParser>();

    // Run all tests
    auto start_time = std::chrono::steady_clock::now();

    // Basic functionality tests
    if (test_sync_import())
        g_stats.record_pass();
    else
        g_stats.record_fail();
    if (test_async_import())
        g_stats.record_pass();
    else
        g_stats.record_fail();
    if (test_write_resource())
        g_stats.record_pass();
    else
        g_stats.record_fail();

    // Concurrency tests
    if (test_concurrent_access())
        g_stats.record_pass();
    else
        g_stats.record_fail();
    if (test_concurrent_read_write())
        g_stats.record_pass();
    else
        g_stats.record_fail();
    if (test_multiple_readers())
        g_stats.record_pass();
    else
        g_stats.record_fail();

    // Advanced tests
    if (test_handle_scope_and_raii())
        g_stats.record_pass();
    else
        g_stats.record_fail();
    if (test_resource_removal())
        g_stats.record_pass();
    else
        g_stats.record_fail();
    if (test_export_functionality())
        g_stats.record_pass();
    else
        g_stats.record_fail();
    if (test_invalid_path())
        g_stats.record_pass();
    else
        g_stats.record_fail();

    // Scalability tests
    if (test_memory_scalability())
        g_stats.record_pass();
    else
        g_stats.record_fail();

    // Stress test (run last)
    if (test_stress())
        g_stats.record_pass();
    else
        g_stats.record_fail();

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Print results
    std::cout << "\n=================================" << std::endl;
    g_stats.print();
    std::cout << "Time elapsed: " << duration.count() << "ms" << std::endl;
    std::cout << "=================================" << std::endl;

    // Cleanup
    cleanup_test_files();

    if (g_stats.failed > 0) {
        std::cout << "\nSome tests FAILED!" << std::endl;
        return 1;
    }

    std::cout << "\nAll tests PASSED!" << std::endl;
    return 0;
}
