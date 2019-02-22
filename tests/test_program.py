import math
import operator
import tempfile

from drgn import cast, container_of, NULL, Object, Program
from drgn.internal.corereader import CoreReader
from drgn.type import IntType, StructType, TypedefType
from tests.test_type import color_type, point_type
from tests.test_typeindex import TypeIndexTestCase, TYPES


class TestObject(TypeIndexTestCase):
    def setUp(self):
        super().setUp()
        def program_object_equality_func(a, b, msg=None):
            if a.prog_ is not b.prog_:
                raise self.failureException(msg or 'objects have different program')
            if a.type_ != b.type_:
                raise self.failureException(msg or f'objects types differ: {a.type_!r} != {b.type_!r}')
            if a.address_ != b.address_:
                a_address = 'None' if a.address_ is None else hex(a.address_)
                b_address = 'None' if b.address_ is None else hex(b.address_)
                raise self.failureException(msg or f'object addresses differ: {a_address} != {b_address}')
            if a._value != b._value:
                raise self.failureException(msg or f'object values differ: {a._value!r} != {b._value!r}')
        self.addTypeEqualityFunc(Object, program_object_equality_func)
        buffer = b'\x01\x00\x00\x00\x02\x00\x00\x00hello\x00\x00\x00'
        segments = [(0, 0xffff0000, 0x0, len(buffer), len(buffer))]
        tmpfile = tempfile.TemporaryFile()
        try:
            tmpfile.write(buffer)
            tmpfile.flush()
            core_reader = CoreReader(tmpfile, segments)
            self.prog = Program(reader=core_reader, type_index=self.type_index,
                                variable_index=None)
        except:
            tmpfile.close()
            raise
    def tearDown(self):
        if hasattr(self, 'prog'):
            self.prog.close()
        super().tearDown()

    def test_constructor(self):
        self.assertRaises(ValueError, Object, self.prog, TYPES['int'])
        self.assertRaises(ValueError, Object, self.prog, TYPES['int'],
                          address=1, value=0xffff0000)

    def test_rvalue(self):
        obj = Object(self.prog, TYPES['int'], value=2**31)
        self.assertEqual(obj.value_(), -2**31)

    def test_cast(self):
        obj = Object(self.prog, TYPES['int'], value=-1)
        cast_obj = cast('unsigned int', obj)
        self.assertEqual(cast_obj,
                         Object(self.prog, TYPES['unsigned int'], value=2**32 - 1))

        obj = Object(self.prog, TYPES['double'], value=1.0)
        self.assertRaises(TypeError, cast, self.type_index.pointer(TYPES['int']), obj)

    def test_str(self):
        obj = Object(self.prog, TYPES['int'], value=1)
        self.assertEqual(str(obj), '(int)1')

        obj = Object(self.prog, self.type_index.pointer(TYPES['void']), value=0xffff0000)
        self.assertEqual(str(obj), '(void *)0xffff0000')

        obj = Object(self.prog, self.type_index.pointer(TYPES['int']), value=0xffff0000)
        self.assertEqual(str(obj), '*(int *)0xffff0000 = 1')

        obj = NULL(self.prog, self.type_index.pointer(TYPES['int']))
        self.assertEqual(str(obj), '(int *)0x0')

        obj = Object(self.prog, self.type_index.pointer(TYPES['char']), value=0xffff0008)
        self.assertEqual(str(obj), '(char *)0xffff0008 = "hello"')

        obj = NULL(self.prog, self.type_index.pointer(TYPES['char']))
        self.assertEqual(str(obj), '(char *)0x0')

        obj = Object(self.prog, self.type_index.pointer(TYPES['char']), value=0xffff000f)
        self.assertEqual(str(obj), '(char *)0xffff000f = ""')

        obj = Object(self.prog, self.type_index.array(TYPES['char'], 8), address=0xffff0008)
        self.assertEqual(str(obj), '(char [8])"hello"')

        obj = Object(self.prog, self.type_index.array(TYPES['char'], 4), address=0xffff0008)
        self.assertEqual(str(obj), '(char [4])"hell"')

    def test_int(self):
        int_obj = Object(self.prog, TYPES['int'], address=0xffff0000)
        bool_obj = Object(self.prog, TYPES['_Bool'], address=0xffff0000)
        for obj in [int_obj, bool_obj]:
            self.assertRaises(ValueError, len, obj)
            with self.assertRaises(ValueError):
                obj[0]
            self.assertRaises(ValueError, next, iter(obj))
            self.assertRaises(ValueError, obj.string_)
            self.assertRaises(ValueError, obj.member_, 'foo')
            self.assertEqual(obj.value_(), 1)
            self.assertTrue(bool(obj))
            # _Bool should be the same because of integer promotions.
            self.assertEqual(-obj, Object(self.prog, TYPES['int'], value=-1))
            self.assertEqual(+obj, Object(self.prog, TYPES['int'], value=1))
            self.assertEqual(~obj, Object(self.prog, TYPES['int'], value=-2))
            self.assertEqual(int(obj), 1)
            self.assertEqual(float(obj), 1.0)
            self.assertEqual(obj.__index__(), 1)
            self.assertEqual(round(obj), 1)
            self.assertEqual(round(obj, 0),
                             Object(self.prog, obj.type_, value=obj.value_()))
            self.assertEqual(math.trunc(obj), 1)
            self.assertEqual(math.floor(obj), 1)
            self.assertEqual(math.ceil(obj), 1)

        self.assertRaisesRegex(AttributeError,
                               "'Object' object has no attribute 'foo'",
                               getattr, int_obj, 'foo')

        obj = Object(self.prog, IntType('int', 4, True, {'const'}),
                     address=0xffff0000)
        self.assertEqual(+obj, Object(self.prog, TYPES['int'], value=1))

    def test_float(self):
        obj = Object(self.prog, TYPES['double'], value=1.5)
        self.assertTrue(bool(obj))
        self.assertEqual(-obj, Object(self.prog, TYPES['double'], value=-1.5))
        self.assertEqual(+obj, Object(self.prog, TYPES['double'], value=1.5))
        with self.assertRaises(TypeError):
            ~obj
        self.assertEqual(int(obj), 1)
        self.assertEqual(float(obj), 1.5)
        self.assertRaises(TypeError, obj.__index__)
        self.assertEqual(round(obj), 2)
        self.assertEqual(round(obj, 0), Object(self.prog, TYPES['double'], value=2))
        self.assertEqual(round(obj, 1), Object(self.prog, TYPES['double'], value=1.5))
        self.assertEqual(math.trunc(obj), 1)
        self.assertEqual(math.floor(obj), 1)
        self.assertEqual(math.ceil(obj), 2)

    def test_pointer(self):
        pointer_type = self.type_index.pointer(TYPES['int'])
        obj = Object(self.prog, pointer_type, value=0xffff0000)
        element0 = Object(self.prog, TYPES['int'], address=0xffff0000)
        element1 = Object(self.prog, TYPES['int'], address=0xffff0004)
        element2 = Object(self.prog, TYPES['int'], address=0xffff0008)
        self.assertRaises(ValueError, len, obj)
        self.assertEqual(obj[0], element0)
        self.assertEqual(obj[1], element1)
        self.assertEqual(obj[2], element2)
        self.assertRaises(ValueError, next, iter(obj))

        pointer_type = self.type_index.pointer(TYPES['char'])
        obj = Object(self.prog, pointer_type, value=0xffff0008)
        self.assertEqual(obj.string_(), b'hello')
        self.assertTrue(bool(obj))

        obj = NULL(self.prog, pointer_type)
        self.assertFalse(bool(obj))
        with self.assertRaises(TypeError):
            +obj
        self.assertRaises(TypeError, int, obj)
        self.assertRaises(TypeError, float, obj)
        self.assertRaises(TypeError, obj.__index__)
        self.assertRaises(TypeError, round, obj)
        self.assertRaises(TypeError, math.trunc, obj)
        self.assertRaises(TypeError, math.floor, obj)
        self.assertRaises(TypeError, math.ceil, obj)

        with self.assertRaises(ValueError):
            obj.member_('foo')

        cast_obj = cast('unsigned long', obj)
        self.assertEqual(cast_obj,
                         Object(self.prog, TYPES['unsigned long'], value=0))
        self.assertRaises(TypeError, obj.__index__)

    def test_array(self):
        array_type = self.type_index.array(TYPES['int'], 2)
        obj = Object(self.prog, array_type, address=0xffff0000)
        element0 = Object(self.prog, TYPES['int'], address=0xffff0000)
        element1 = Object(self.prog, TYPES['int'], address=0xffff0004)
        element2 = Object(self.prog, TYPES['int'], address=0xffff0008)
        self.assertEqual(len(obj), 2)
        self.assertEqual(obj[0], element0)
        self.assertEqual(obj[1], element1)
        elements = list(obj)
        self.assertEqual(len(elements), 2)
        self.assertEqual(elements[0], element0)
        self.assertEqual(elements[1], element1)
        self.assertEqual(obj.value_(), [1, 2])

        array_type = self.type_index.array(TYPES['int'], None)
        obj = Object(self.prog, array_type, address=0xffff0000)
        self.assertRaises(ValueError, len, obj)
        self.assertEqual(obj[0], element0)
        self.assertEqual(obj[1], element1)
        self.assertEqual(obj[2], element2)
        self.assertRaises(ValueError, next, iter(obj))

        array_type = self.type_index.array(TYPES['char'], 2)
        obj = Object(self.prog, array_type, address=0xffff0008)
        self.assertEqual(obj.string_(), b'he')

        array_type = self.type_index.array(TYPES['char'], 8)
        obj = Object(self.prog, array_type, address=0xffff0008)
        self.assertEqual(obj.string_(), b'hello')

    def test_struct(self):
        struct_obj = Object(self.prog, point_type, address=0xffff0000)
        typedef_type = TypedefType('POINT', point_type)
        typedef_obj = Object(self.prog, typedef_type, address=0xffff0000)
        pointer_type = self.type_index.pointer(point_type)
        pointer_obj = Object(self.prog, pointer_type, value=0xffff0000)
        typedef_pointer_type = self.type_index.pointer(typedef_type)
        typedef_pointer_obj = Object(self.prog, typedef_pointer_type,
                                     value=0xffff0000)
        element0 = Object(self.prog, TYPES['int'], address=0xffff0000)
        element1 = Object(self.prog, TYPES['int'], address=0xffff0004)

        for obj in [struct_obj, typedef_obj, pointer_obj, typedef_pointer_obj]:
            self.assertEqual(obj.x, element0)
            self.assertEqual(obj.y, element1)
            self.assertEqual(obj.member_('x'),
                             Object(self.prog, TYPES['int'], address=0xffff0000))
            self.assertEqual(obj.member_('y'),
                             Object(self.prog, TYPES['int'], address=0xffff0004))
            self.assertRaisesRegex(AttributeError,
                                   "'struct point' has no member 'z'",
                                   getattr, obj, 'z')
            self.assertRaisesRegex(ValueError,
                                   "'struct point' has no member 'z'",
                                   obj.member_, 'z')
            self.assertIn('x', dir(obj))
            self.assertIn('y', dir(obj))
            self.assertTrue(hasattr(obj, 'x'))
            self.assertTrue(hasattr(obj, 'y'))
            self.assertFalse(hasattr(obj, 'z'))

        element1_ptr = element1.address_of_()
        self.assertEqual(element1_ptr,
                         Object(self.prog,
                                self.type_index.pointer(TYPES['int']),
                                value=0xffff0004))
        self.assertEqual(container_of(element1_ptr, point_type, 'y'), pointer_obj)
        self.assertEqual(container_of(element1_ptr, typedef_type, 'y'),
                         typedef_pointer_obj)

        struct_type = StructType('test', 8, [
            ('address_', 0, lambda: TYPES['unsigned long']),
        ])
        struct_obj = Object(self.prog, struct_type, address=0xffff0000)
        self.assertEqual(struct_obj.address_, 0xffff0000)
        self.assertEqual(struct_obj.member_('address_'),
                         Object(self.prog, TYPES['unsigned long'],
                                address=0xffff0000))
    def test_enum(self):
        enum_obj = Object(self.prog, color_type, value=0)
        self.assertEqual(enum_obj.value_(), color_type.enum.RED)
        self.assertIsInstance(enum_obj.value_(), color_type.enum)
        self.assertEqual([1, 2, 3][enum_obj], 1)

    def test_relational(self):
        one = Object(self.prog, TYPES['int'], value=1)
        two = Object(self.prog, TYPES['int'], value=2)
        three = Object(self.prog, TYPES['int'], value=3)
        ptr0 = Object(self.prog, self.type_index.pointer(TYPES['int']),
                      value=0xffff0000)
        ptr1 = Object(self.prog, self.type_index.pointer(TYPES['int']),
                      value=0xffff0004)

        self.assertTrue(one < two)
        self.assertFalse(two < two)
        self.assertFalse(three < two)
        self.assertTrue(ptr0 < ptr1)

        self.assertTrue(one <= two)
        self.assertTrue(two <= two)
        self.assertFalse(three <= two)
        self.assertTrue(ptr0 <= ptr1)

        self.assertTrue(one == one)
        self.assertFalse(one == two)
        self.assertFalse(ptr0 == ptr1)

        self.assertFalse(one != one)
        self.assertTrue(one != two)
        self.assertTrue(ptr0 != ptr1)

        self.assertFalse(one > two)
        self.assertFalse(two > two)
        self.assertTrue(three > two)
        self.assertFalse(ptr0 > ptr1)

        self.assertFalse(one >= two)
        self.assertTrue(two >= two)
        self.assertTrue(three >= two)
        self.assertFalse(ptr0 >= ptr1)

        negative_one = Object(self.prog, TYPES['int'], value=-1)
        unsigned_zero = Object(self.prog, TYPES['unsigned int'], value=0)
        # The usual arithmetic conversions convert -1 to an unsigned int.
        self.assertFalse(negative_one < unsigned_zero)

        self.assertTrue(Object(self.prog, TYPES['int'], value=1) ==
                        Object(self.prog, TYPES['_Bool'], value=1))

        self.assertRaises(TypeError, operator.lt, ptr0, one)

    def _test_arithmetic(self, op, lhs, rhs, result, integral=True,
                         floating_point=False):
        def INT(value):
            return Object(self.prog, TYPES['int'], value=value)
        def LONG(value):
            return Object(self.prog, TYPES['long'], value=value)
        def DOUBLE(value):
            return Object(self.prog, TYPES['double'], value=value)

        if integral:
            self.assertEqual(op(INT(lhs), INT(rhs)), INT(result))
            self.assertEqual(op(INT(lhs), LONG(rhs)), LONG(result))
            self.assertEqual(op(LONG(lhs), INT(rhs)), LONG(result))
            self.assertEqual(op(LONG(lhs), LONG(rhs)), LONG(result))
            self.assertEqual(op(INT(lhs), rhs), INT(result))
            self.assertEqual(op(LONG(lhs), rhs), LONG(result))
            self.assertEqual(op(lhs, INT(rhs)), INT(result))
            self.assertEqual(op(lhs, LONG(rhs)), LONG(result))

        if floating_point:
            self.assertEqual(op(DOUBLE(lhs), DOUBLE(rhs)), DOUBLE(result))
            self.assertEqual(op(DOUBLE(lhs), INT(rhs)), DOUBLE(result))
            self.assertEqual(op(INT(lhs), DOUBLE(rhs)), DOUBLE(result))
            self.assertEqual(op(DOUBLE(lhs), float(rhs)), DOUBLE(result))
            self.assertEqual(op(float(lhs), DOUBLE(rhs)), DOUBLE(result))
            self.assertEqual(op(float(lhs), INT(rhs)), DOUBLE(result))
            self.assertEqual(op(INT(lhs), float(rhs)), DOUBLE(result))

    def _test_pointer_type_errors(self, op):
        def INT(value):
            return Object(self.prog, TYPES['int'], value=value)
        def POINTER(value):
            return Object(self.prog, self.type_index.pointer(TYPES['int']),
                          value=value)

        self.assertRaises(TypeError, op, INT(1), POINTER(1))
        self.assertRaises(TypeError, op, POINTER(1), INT(1))
        self.assertRaises(TypeError, op, POINTER(1), POINTER(1))

    def _test_floating_type_errors(self, op):
        def INT(value):
            return Object(self.prog, TYPES['int'], value=value)
        def DOUBLE(value):
            return Object(self.prog, TYPES['double'], value=value)

        self.assertRaises(TypeError, op, INT(1), DOUBLE(1))
        self.assertRaises(TypeError, op, DOUBLE(1), INT(1))
        self.assertRaises(TypeError, op, DOUBLE(1), DOUBLE(1))

    def _test_shift(self, op, lhs, rhs, result):
        def BOOL(value):
            return Object(self.prog, TYPES['_Bool'], value=value)
        def INT(value):
            return Object(self.prog, TYPES['int'], value=value)
        def LONG(value):
            return Object(self.prog, TYPES['long'], value=value)

        self.assertEqual(op(INT(lhs), INT(rhs)), INT(result))
        self.assertEqual(op(INT(lhs), LONG(rhs)), INT(result))
        self.assertEqual(op(LONG(lhs), INT(rhs)), LONG(result))
        self.assertEqual(op(LONG(lhs), LONG(rhs)), LONG(result))
        self.assertEqual(op(INT(lhs), rhs), INT(result))
        self.assertEqual(op(LONG(lhs), rhs), LONG(result))
        self.assertEqual(op(lhs, INT(rhs)), INT(result))
        self.assertEqual(op(lhs, LONG(rhs)), INT(result))

        self._test_pointer_type_errors(op)
        self._test_floating_type_errors(op)

    def test_add(self):
        self._test_arithmetic(operator.add, 2, 2, 4, floating_point=True)

        one = Object(self.prog, TYPES['int'], value=1)
        ptr = Object(self.prog, self.type_index.pointer(TYPES['int']),
                     value=0xffff0000)
        ptr1 = Object(self.prog, self.type_index.pointer(TYPES['int']),
                      value=0xffff0004)
        self.assertEqual(ptr + one, ptr1)
        self.assertEqual(one + ptr, ptr1)
        self.assertEqual(ptr + 1, ptr1)
        self.assertEqual(1 + ptr, ptr1)
        self.assertRaises(TypeError, operator.add, ptr, ptr)
        self.assertRaises(TypeError, operator.add, ptr, 2.0)
        self.assertRaises(TypeError, operator.add, 2.0, ptr)

    def test_sub(self):
        self._test_arithmetic(operator.sub, 4, 2, 2, floating_point=True)

        ptr = Object(self.prog, self.type_index.pointer(TYPES['int']),
                     value=0xffff0000)
        ptr1 = Object(self.prog, self.type_index.pointer(TYPES['int']),
                      value=0xffff0004)
        self.assertEqual(ptr1 - ptr,
                         Object(self.prog, TYPES['ptrdiff_t'], value=1))
        self.assertEqual(ptr - ptr1,
                         Object(self.prog, TYPES['ptrdiff_t'], value=-1))
        self.assertEqual(ptr - 0, ptr)
        self.assertEqual(ptr1 - 1, ptr)
        self.assertRaises(TypeError, operator.sub, 1, ptr)
        self.assertRaises(TypeError, operator.sub, ptr, 1.0)

    def test_mul(self):
        self._test_arithmetic(operator.mul, 2, 3, 6, floating_point=True)
        self._test_pointer_type_errors(operator.mul)

    def test_div(self):
        self._test_arithmetic(operator.truediv, 6, 3, 2, floating_point=True)

        # Make sure we do integer division for integer operands.
        self._test_arithmetic(operator.truediv, 3, 2, 1)

        # Make sure we truncate towards zero (Python truncates towards negative
        # infinity).
        self._test_arithmetic(operator.truediv, -1, 2, 0)
        self._test_arithmetic(operator.truediv, 1, -2, 0)

        self._test_pointer_type_errors(operator.mul)

    def test_mod(self):
        self._test_arithmetic(operator.mod, 4, 2, 0)

        # Make sure the modulo result has the sign of the dividend (Python uses
        # the sign of the divisor).
        self._test_arithmetic(operator.mod, 1, 26, 1)
        self._test_arithmetic(operator.mod, 1, -26, 1)
        self._test_arithmetic(operator.mod, -1, 26, -1)
        self._test_arithmetic(operator.mod, -1, -26, -1)

        self._test_pointer_type_errors(operator.mod)
        self._test_floating_type_errors(operator.mod)

    def test_lshift(self):
        self._test_shift(operator.lshift, 2, 3, 16)

    def test_rshift(self):
        self._test_shift(operator.rshift, 16, 3, 2)

    def test_and(self):
        self._test_arithmetic(operator.and_, 1, 3, 1)
        self._test_pointer_type_errors(operator.and_)
        self._test_floating_type_errors(operator.and_)

    def test_xor(self):
        self._test_arithmetic(operator.xor, 1, 3, 2)
        self._test_pointer_type_errors(operator.xor)
        self._test_floating_type_errors(operator.xor)

    def test_or(self):
        self._test_arithmetic(operator.or_, 1, 3, 3)
        self._test_pointer_type_errors(operator.or_)
        self._test_floating_type_errors(operator.or_)
