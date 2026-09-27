// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file metrics.cpp
// @brief 指标注册表实现（Prometheus 文本暴露格式）。

#include "corodb/common/metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace corodb {

    Histogram::Histogram(std::vector<double> upper_bounds) : bounds_(std::move(upper_bounds)) {
        // 桶界须严格升序；+Inf 桶由 count_ 隐含，不进 bounds_。
        std::sort(bounds_.begin(), bounds_.end());
        bounds_.erase(std::unique(bounds_.begin(), bounds_.end()), bounds_.end());
        nbuckets_ = bounds_.size() + 1;
        cumulative_ = std::make_unique<std::atomic<uint64_t>[]>(nbuckets_);
        for (std::size_t i = 0; i < nbuckets_; ++i)
            cumulative_[i].store(0, std::memory_order_relaxed);
    }

    void Histogram::observe(double v) noexcept {
        // 上界以上不设桶：定位第一个 >= v 的桶界之后全部累计（Prometheus 累积语义）。
        std::size_t first_ge = bounds_.size();
        for (std::size_t i = 0; i < bounds_.size(); ++i) {
            if (v <= bounds_[i]) {
                first_ge = i;
                break;
            }
        }
        for (std::size_t i = first_ge; i < nbuckets_; ++i)
            cumulative_[i].fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(v, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    Metrics& Metrics::instance() {
        static Metrics m;
        return m;
    }

    Counter& Metrics::counter(const std::string& name, const std::string& labels, const std::string& help) {
        std::lock_guard lk(mutex_);
        auto& e = metrics_[name + labels];
        e.name = name;
        e.labels = labels;
        e.kind = Entry::Kind::Counter;
        if (!help.empty())
            e.help = help;
        if (!e.counter)
            e.counter = std::make_unique<Counter>();
        return *e.counter;
    }

    Gauge& Metrics::gauge(const std::string& name, const std::string& labels, const std::string& help) {
        std::lock_guard lk(mutex_);
        auto& e = metrics_[name + labels];
        e.name = name;
        e.labels = labels;
        e.kind = Entry::Kind::Gauge;
        if (!help.empty())
            e.help = help;
        if (!e.gauge)
            e.gauge = std::make_unique<Gauge>();
        return *e.gauge;
    }

    Histogram& Metrics::histogram(const std::string& name, const std::vector<double>& buckets,
                                  const std::string& help) {
        std::lock_guard lk(mutex_);
        auto& e = metrics_[name];
        e.name = name;
        e.kind = Entry::Kind::Histogram;
        if (!help.empty())
            e.help = help;
        if (!e.histogram)
            e.histogram = std::make_unique<Histogram>(buckets);
        return *e.histogram;
    }

    std::string Metrics::render_prometheus() const {
        // 快照指针集合（锁内），渲染在锁外（值均为原子读）。
        std::vector<const Entry*> entries;
        {
            std::lock_guard lk(mutex_);
            entries.reserve(metrics_.size());
            for (const auto& [key, e]: metrics_)
                entries.push_back(&e);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Entry* a, const Entry* b) { return a->name + a->labels < b->name + b->labels; });

        std::ostringstream oss;
        std::string last_help_name;
        for (const Entry* e: entries) {
            if (e->name != last_help_name && !e->help.empty()) {
                const char* kind = e->kind == Entry::Kind::Counter
                                       ? "counter"
                                       : (e->kind == Entry::Kind::Gauge ? "gauge" : "histogram");
                oss << "# HELP " << e->name << ' ' << e->help << "\n";
                oss << "# TYPE " << e->name << ' ' << kind << "\n";
                last_help_name = e->name;
            }
            switch (e->kind) {
                case Entry::Kind::Counter:
                    oss << e->name << e->labels << ' ' << e->counter->value() << "\n";
                    break;
                case Entry::Kind::Gauge:
                    oss << e->name << e->labels << ' ' << e->gauge->value() << "\n";
                    break;
                case Entry::Kind::Histogram:
                    for (std::size_t i = 0; i <= e->histogram->upper_bounds().size(); ++i) {
                        oss << e->name << "_bucket" << e->labels;
                        if (i < e->histogram->upper_bounds().size()) {
                            oss << "{le=\"" << e->histogram->upper_bounds()[i] << "\"}";
                        } else {
                            oss << "{le=\"+Inf\"}";
                        }
                        oss << ' ' << e->histogram->bucket_count(i) << "\n";
                    }
                    {
                        char buf[64];
                        snprintf(buf, sizeof buf, "%.17g", e->histogram->sum());
                        oss << e->name << "_sum" << e->labels << ' ' << buf << "\n";
                        oss << e->name << "_count" << e->labels << ' ' << e->histogram->count() << "\n";
                    }
                    break;
            }
        }
        return oss.str();
    }

    std::size_t Metrics::size() const {
        std::lock_guard lk(mutex_);
        return metrics_.size();
    }

} // namespace corodb
