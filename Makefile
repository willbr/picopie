all:
	python -m picopie examples/00.py

wall:
	watchexec -cr "make all"

install:
	python -m pip install -e .

