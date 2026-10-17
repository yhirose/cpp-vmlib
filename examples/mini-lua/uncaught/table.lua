-- An error object that is neither a string nor a number: lua reports
-- "(error object is a table value)".
error({code = 7})
