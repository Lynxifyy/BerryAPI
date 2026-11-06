-- BerryAPI example: the basics, as a runnable test suite.
--
-- `make check-examples` runs this file through `berry-run test`, so every line
-- here is verified in CI. Run it directly with:
--   ./build/berry-run test examples/basics.luau

local M = {}

-- A script exports a table of functions. The runner discovers and calls each
-- `test_*` member, so a test is just an exported function.
function M.test_arithmetic()
	test.eq(2 + 2, 4)
	test.eq(10 / 4, 2.5)
end

function M.test_strings()
	local s = "berry" .. "-" .. "api"
	test.eq(s, "berry-api")
	test.eq(#s, 9)
end

function M.test_tables()
	local t = { a = 1, b = 2 }
	test.eq(t.a + t.b, 3)
	t.c = 3
	test.eq(t.c, 3)
end

-- `test.near` compares floats with a tolerance, which is what a real check on
-- computed geometry needs.
function M.test_float_tolerance()
	test.near(0.1 + 0.2, 0.3, 1e-9)
end

-- An expected-failure test documents a known limitation without failing the
-- suite. A leading `xfail_` marks it.
function M.test_xfail_known_gap()
	test.fail("this path is not implemented yet")
end

return M
