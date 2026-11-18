.PHONY: check format format-check test asan tsan tidy cppcheck analyze embedded

check:
	python3 tools/quality/check.py all

format format-check test asan tsan tidy cppcheck analyze:
	python3 tools/quality/check.py $@

embedded:
	python3 tools/quality/embedded.py
