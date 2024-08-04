import sys
import unwind

if __name__ == '__main__':
    print('hello main')
else:
    print('hello lib')

ast = unwind.unwind_file(sys.argv[1])
#print(ast)

head, *expressions = ast
assert head == 'module'

for expr in expressions:
    print(expr)

