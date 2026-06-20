all:
	@mkdir -p build
	@cd build && cmake .. && cmake --build .

run: all
	@cd build && ./spreadsheet.exe "C:\tools\msys64\home\Kumar Saurav\ExcelProMax\cheatsheet\data\data"

clean:
	@rm -rf build
