#!/usr/bin/env bash
#
# k6 load test for OS Gateway
#

import http from 'k6/http';
import { check, sleep } from 'k6';
import { Rate, Trend } from 'k6/metrics';

// Custom metrics
const errorRate = new Rate('errors');
const latency = new Trend('latency');

// Test configuration
export const options = {
    stages: [
        { duration: '30s', target: 100 },  // Ramp up to 100 users
        { duration: '1m', target: 100 },   // Stay at 100 users
        { duration: '30s', target: 200 },  // Ramp up to 200 users
        { duration: '1m', target: 200 },   // Stay at 200 users
        { duration: '30s', target: 0 },    // Ramp down
    ],
    thresholds: {
        http_req_duration: ['p(95)<500'],  // 95% of requests should be below 500ms
        errors: ['rate<0.01'],              // Error rate should be below 1%
    },
};

const BASE_URL = __ENV.GATEWAY_URL || 'http://localhost:8080';

export default function () {
    // Test health endpoint
    let healthRes = http.get(`${BASE_URL}/health`);
    check(healthRes, {
        'health status is 200': (r) => r.status === 200,
    });

    // Test API endpoint
    let apiRes = http.get(`${BASE_URL}/api/v1/test`, {
        headers: {
            'Content-Type': 'application/json',
        },
    });

    let success = check(apiRes, {
        'api status is 200 or 429': (r) => r.status === 200 || r.status === 429,
        'api response time < 500ms': (r) => r.timings.duration < 500,
    });

    errorRate.add(!success);
    latency.add(apiRes.timings.duration);

    // Test POST request
    let payload = JSON.stringify({
        name: 'test',
        value: Math.random(),
    });

    let postRes = http.post(`${BASE_URL}/api/v1/data`, payload, {
        headers: {
            'Content-Type': 'application/json',
        },
    });

    check(postRes, {
        'post status is 2xx or 429': (r) => (r.status >= 200 && r.status < 300) || r.status === 429,
    });

    sleep(0.1);
}

export function handleSummary(data) {
    return {
        'summary.json': JSON.stringify(data),
    };
}
