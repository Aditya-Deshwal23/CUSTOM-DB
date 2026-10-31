# Dashboard Lighthouse report

The committed [summary report](./lighthouse-report.json) was captured on
2026-10-31 from the real Crow binary serving the dashboard. Lighthouse 13.6.0
reported **Performance 100** and **Accessibility 100**, both above the CI
acceptance threshold of 90. The report also records the engine build SHA,
Core Web Vitals, and key accessibility audit results.

Re-run the same audit with:

```sh
python3 scripts/run_lighthouse.py \
  --binary build-debug/relational_db \
  --output docs/lighthouse-report.json
```

CI runs the script against the real Debug binary and fails if either category
falls below 90. The report is a point-in-time measurement, not a guarantee for
other devices or deployment environments.
