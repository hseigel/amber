/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

import java.lang.invoke.*;
import java.lang.invoke.constant.*;
import java.util.*;

import org.testng.annotations.Test;

import static org.testng.Assert.*;

/**
 * @test
 * @compile -XDfolding=false ConstantUtilsTest.java
 * @run testng ConstantUtilsTest
 * @summary unit tests for methods of java.lang.invoke.constant.ConstantUtils that are not covered by other unit tests
 */
@Test
public class ConstantUtilsTest {
    private static ClassDesc thisClass = ClassDesc.of("MethodHandleRefTest");

    public void testValidateMemberName() {
        try {
            ConstantUtils.validateMemberName(null);
            fail("");
        } catch (NullPointerException e) {
            // good
        }

        try {
            ConstantUtils.validateMemberName("");
            fail("");
        } catch (IllegalArgumentException e) {
            // good
        }

        List<String> badNames = List.of(".", ";", "[", "/", "<", ">");
        for (String n : badNames) {
            try {
                ConstantUtils.validateMemberName(n);
                fail(n);
            } catch (IllegalArgumentException e) {
                // good
            }
        }
    }

    public void testSymbolizeHelper() {
        ConstantMethodHandleDesc mh = MethodHandleDesc.of(MethodHandleDesc.Kind.VIRTUAL, ConstantDescs.CR_String, "isEmpty", "()Z");
        try {
            ConstantUtils.symbolizeHelper(mh, null, "");
            fail("");
        } catch (NullPointerException e) {
            // good
        }

        try {
            ConstantUtils.symbolizeHelper(null, ConstantDescs.CR_ClassDesc, "");
            fail("");
        } catch (NullPointerException e) {
            // good
        }

        try {
            ConstantUtils.symbolizeHelper(null, null, "");
            fail("");
        } catch (NullPointerException e) {
            // good
        }
    }
}
