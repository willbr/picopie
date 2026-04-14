PYTHON = PYTHONPATH=$(HOME)/src/unwind python3

all:
	$(PYTHON) -m picopie examples/00.py

test:
	$(PYTHON) -m picopie examples/add.py 3 5 | grep -q "= 8"
	$(PYTHON) -m picopie examples/fib.py 10 | grep -q "= 55"
	$(PYTHON) -m picopie examples/abs.py 7 | grep -q "= 7"
	$(PYTHON) -m picopie examples/abs.py -- -7 | grep -q "= 7"
	$(PYTHON) -m picopie examples/sum_to.py 100 | grep -q "= 5050"
	@echo "all tests passed"

test-d:
	$(PYTHON) -m picopie -d examples/add.py 3 5
	$(PYTHON) -m picopie -d examples/fib.py 10
	$(PYTHON) -m picopie -d examples/abs.py 7
	$(PYTHON) -m picopie -d examples/abs.py -- -7
	$(PYTHON) -m picopie -d examples/sum_to.py 100

wall:
	watchexec -cr "make all"

install:
	python -m pip install -e .

