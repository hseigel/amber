/*
 * Copyright (c) 2012, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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


package org.graalvm.compiler.nodes;

import org.graalvm.compiler.core.common.type.StampPair;
import org.graalvm.compiler.graph.NodeClass;
import org.graalvm.compiler.nodeinfo.NodeInfo;

import jdk.vm.ci.code.CallingConvention;
import jdk.vm.ci.meta.JavaType;
import jdk.vm.ci.meta.ResolvedJavaMethod;

@NodeInfo
public class IndirectCallTargetNode extends LoweredCallTargetNode {
    public static final NodeClass<IndirectCallTargetNode> TYPE = NodeClass.create(IndirectCallTargetNode.class);

    @Input protected ValueNode computedAddress;

    public IndirectCallTargetNode(ValueNode computedAddress, ValueNode[] arguments, StampPair returnStamp, JavaType[] signature, ResolvedJavaMethod target,
                    CallingConvention.Type callType,
                    InvokeKind invokeKind) {
        this(TYPE, computedAddress, arguments, returnStamp, signature, target, callType, invokeKind);
    }

    protected IndirectCallTargetNode(NodeClass<? extends IndirectCallTargetNode> c, ValueNode computedAddress, ValueNode[] arguments, StampPair returnStamp,
                    JavaType[] signature,
                    ResolvedJavaMethod target, CallingConvention.Type callType, InvokeKind invokeKind) {
        super(c, arguments, returnStamp, signature, target, callType, invokeKind);
        this.computedAddress = computedAddress;
    }

    public ValueNode computedAddress() {
        return computedAddress;
    }

    @Override
    public String targetName() {
        if (targetMethod() == null) {
            return "[unknown]";
        }
        return targetMethod().format("Indirect#%h.%n");
    }
}
