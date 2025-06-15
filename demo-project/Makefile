.PHONY: build test clean run

build:
\tgoo build -o bin/demo-project main.goo

test:
\tgoo test ./...

run: build
\t./bin/demo-project

clean:
\trm -rf bin/ build/ dist/

install: build
\tcp bin/demo-project /usr/local/bin/
