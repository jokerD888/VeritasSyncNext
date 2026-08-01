import type { HTMLAttributes } from "react";
import { cn } from "@/lib/utils";

export function Badge({ className, ...props }: HTMLAttributes<HTMLSpanElement>) {
  return <span className={cn("inline-flex items-center rounded-md border border-[#d5dfed] bg-[#f6f9fd] px-2 py-0.5 font-mono text-[10px] font-medium tracking-wide text-[#5a6d88]", className)} {...props} />;
}
